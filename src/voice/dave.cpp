// C++ bridge to the mlspp MLS engine + the mbedTLS HPKE shim, exposed to the
// plain-C voice gateway through the extern "C" surface in dave.h.

#include "voice/dave.h"
#include "voice/dave_frame.h"

#include <mls/crypto.h>
#include <mls/core_types.h>
#include <mls/credential.h>
#include <mls/messages.h>
#include <mls/key_schedule.h>
#include <mls/state.h>
#include <tls/tls_syntax.h>

#include <hpke/hpke.h>
#include <hpke/random.h>
#include <hpke/signature.h>

#include <bytes/bytes.h>

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <list>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" void console_logf(const char *fmt, ...);
extern "C" void console_dbgf(const char *fmt, ...);

namespace h = mlspp::hpke;
using mlspp::bytes_ns::bytes;
using mlspp::bytes_ns::from_ascii;


static int
selftest_hpke()
{
  int fails = 0;

  // 1. ECDSA-P256 sign / verify, including tamper rejection.
  const auto& sig = h::Signature::get<h::Signature::ID::P256_SHA256>();
  auto sk = sig.generate_key_pair();
  auto pk = sk->public_key();
  auto msg = from_ascii("cordless dave selftest message");
  auto signature = sig.sign(msg, *sk);
  if (!sig.verify(msg, signature, *pk)) {
    fails |= 1;
  }
  auto tampered = msg;
  tampered.at(0) ^= 0xFF;
  if (sig.verify(tampered, signature, *pk)) {
    fails |= 2;  // a modified message must not verify
  }

  // 2. DHKEM-P256 encap / decap must agree on the shared secret.
  const auto& kem = h::KEM::get<h::KEM::ID::DHKEM_P256_SHA256>();
  auto skR = kem.derive_key_pair(h::random_bytes(32));
  auto pkR = skR->public_key();
  auto [ss_s, enc] = kem.encap(*pkR);
  auto ss_r = kem.decap(enc, *skR);
  if (ss_s != ss_r) {
    fails |= 4;
  }

  // 3. Full HPKE base-mode seal/open.
  auto hpke =
    h::HPKE(h::KEM::ID::DHKEM_P256_SHA256, h::KDF::ID::HKDF_SHA256,
            h::AEAD::ID::AES_128_GCM);
  auto info = from_ascii("cordless-dave");
  auto sender = hpke.setup_base_s(*pkR, info);
  auto receiver = hpke.setup_base_r(sender.first, *skR, info);
  auto aad = from_ascii("frame-aad");
  auto pt = from_ascii("end-to-end encrypted voice");
  auto ct = sender.second.seal(aad, pt);
  auto opened = receiver.open(aad, ct);
  if (!opened.has_value() || opened.value() != pt) {
    fails |= 8;
  }

  return fails;
}

static int
selftest_frame()
{
  // DAVE frame codec round-trip.
  uint8_t key[16];
  for (int i = 0; i < 16; i++) {
    key[i] = (uint8_t)(0x30 + i);
  }
  const char *plain = "opus-frame-payload-0123456789abcdef";
  size_t plen = std::strlen(plain);

  uint8_t wrapped[128];
  int wlen = dave_frame_encrypt(
    key, 0x11223344u, (const uint8_t *)plain, plen, wrapped, sizeof(wrapped));
  if (wlen < 0) {
    return 1;
  }

  uint32_t nonce = 0;
  if (dave_frame_peek_nonce(wrapped, (size_t)wlen, &nonce) != 0 ||
      nonce != 0x11223344u) {
    return 2;
  }

  uint8_t out[128];
  int olen =
    dave_frame_decrypt(key, wrapped, (size_t)wlen, out, sizeof(out), nullptr);
  if (olen != (int)plen || std::memcmp(out, plain, plen) != 0) {
    return 4;
  }

  // A corrupted tag must fail authentication.
  wrapped[plen] ^= 0xFF;
  if (dave_frame_decrypt(key, wrapped, (size_t)wlen, out, sizeof(out),
                         nullptr) >= 0) {
    return 8;
  }

  return 0;
}

static int selftest_mls_protocol();

extern "C" int
dave_selftest(void)
{
  int fails = 0;
  try {
    fails |= selftest_hpke();
    int frame = selftest_frame();
    if (frame != 0) {
      console_logf("[dave] selftest frame codec failed (0x%x)", frame);
      fails |= 32;
    }
    int mls = selftest_mls_protocol();
    if (mls != 0) {
      console_logf("[dave] selftest mls protocol failed (0x%x)", mls);
      fails |= 64;
    }
  } catch (const std::exception& e) {
    console_logf("[dave] selftest threw: %s", e.what());
    fails |= 16;
  } catch (...) {
    fails |= 16;
  }

  if (fails == 0) {
    console_logf("[dave] crypto selftest PASS (ECDSA-P256, DHKEM-P256, HPKE, "
                 "frame codec, MLS DAVE)");
  } else {
    console_logf("[dave] crypto selftest FAIL mask=0x%x", fails);
  }
  return fails;
}


namespace {

// MLS credential identity = big-endian u64 user id.
bytes
be64(uint64_t v)
{
  std::vector<uint8_t> b(8);
  for (int i = 0; i < 8; i++) {
    b[7 - i] = (uint8_t)(v >> (8 * i));
  }
  return b;
}

// Secure Frames exporter context = little-endian u64 user id.
bytes
le64(uint64_t v)
{
  std::vector<uint8_t> b(8);
  for (int i = 0; i < 8; i++) {
    b[i] = (uint8_t)(v >> (8 * i));
  }
  return b;
}

bytes
to_bytes(const uint8_t* p, size_t n)
{
  return std::vector<uint8_t>(p, p + n);
}

uint64_t
parse_uid(const char* s)
{
  return s ? (uint64_t)std::strtoull(s, nullptr, 10) : 0;
}

struct QueuedProposal
{
  mlspp::ValidatedContent content;
  bytes ref;
};

struct OutboundCommit
{
  bytes raw_commit;
  mlspp::State next_state;
};

constexpr size_t kMaxOutboundCommits = 16;

}  // namespace

struct dave_session
{
  std::string user_id;
  uint64_t user_id_u64;
  uint64_t group_id;
  mlspp::CipherSuite suite;
  mlspp::SignaturePrivateKey sig_priv;
  mlspp::HPKEPrivateKey init_priv;  // KeyPackage.init_key (regenerated per KP)
  mlspp::HPKEPrivateKey leaf_priv;  // LeafNode.encryption_key
  std::optional<mlspp::ExternalSender> ext_sender;
  std::optional<mlspp::KeyPackage> my_kp;
  bool want_epoch1 = false;
  std::optional<mlspp::State> pending_group;  // sole-member epoch 1 group
  std::optional<mlspp::State> current_state;  // established MLS epoch state
  std::optional<mlspp::State> state_with_proposals;
  std::vector<QueuedProposal> proposal_queue;
  std::list<OutboundCommit> outbound_commits;

  dave_session(const std::string& uid, uint64_t gid)
    : user_id(uid)
    , user_id_u64(parse_uid(uid.c_str()))
    , group_id(gid)
    , suite(mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256)
    , sig_priv(mlspp::SignaturePrivateKey::generate(suite))
    , init_priv(mlspp::HPKEPrivateKey::generate(suite))
    , leaf_priv(mlspp::HPKEPrivateKey::generate(suite))
  {
  }

  mlspp::LeafNode build_leaf() const
  {
    auto caps = mlspp::Capabilities::create_default();
    caps.cipher_suites = { mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256 };
    caps.credentials = { mlspp::CredentialType::basic };
    auto cred = mlspp::Credential::basic(be64(user_id_u64));
    return mlspp::LeafNode(suite,
                           leaf_priv.public_key,
                           sig_priv.public_key,
                           cred,
                           caps,
                           mlspp::Lifetime::create_default(),
                           mlspp::ExtensionList{},
                           sig_priv);
  }

  mlspp::ExtensionList group_extensions() const
  {
    mlspp::ExtensionList exts;
    if (ext_sender) {
      exts.add(mlspp::ExternalSendersExtension{
        { { ext_sender->signature_key, ext_sender->credential } } });
    }
    return exts;
  }

  void clear_pending()
  {
    pending_group.reset();
    state_with_proposals.reset();
    proposal_queue.clear();
    outbound_commits.clear();
  }

  bool create_pending_group()
  {
    if (!ext_sender) {
      return false;
    }
    auto leaf = build_leaf();
    mlspp::State st(be64(group_id),
                    suite,
                    leaf_priv,
                    sig_priv,
                    leaf,
                    group_extensions());
    pending_group = std::move(st);
    console_dbgf("[dave] created pending MLS group (epoch 1)");
    return true;
  }
};

extern "C" dave_session*
dave_session_create(const char* self_user_id, uint64_t group_id)
{
  try {
    auto* s = new dave_session(self_user_id ? self_user_id : "", group_id);
    console_dbgf("[dave] session created (cs2 P256, user=%s)", s->user_id.c_str());
    return s;
  } catch (const std::exception& e) {
    console_logf("[dave] session create failed: %s", e.what());
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

extern "C" void
dave_session_free(dave_session* s)
{
  delete s;
}

extern "C" int
dave_established(dave_session* s)
{
  return (s && s->current_state) ? 1 : 0;
}

extern "C" void
dave_reset(dave_session* s)
{
  if (s) {
    s->current_state.reset();
    s->my_kp.reset();
    s->want_epoch1 = false;
    s->clear_pending();
  }
}

extern "C" int
dave_on_external_sender(dave_session* s, const uint8_t* pkg, size_t len)
{
  if (!s || !pkg) {
    return -1;
  }
  try {
    s->ext_sender = mlspp::tls::get<mlspp::ExternalSender>(to_bytes(pkg, len));
    if (s->want_epoch1 && !s->current_state && !s->pending_group) {
      s->create_pending_group();
    }
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] external_sender parse: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_make_key_package(dave_session* s, uint8_t* out, size_t* len, size_t cap)
{
  if (!s || !out || !len) {
    return -1;
  }
  try {
    // KeyPackages are single-use; refresh init_priv each time.
    s->init_priv = mlspp::HPKEPrivateKey::generate(s->suite);
    auto leaf = s->build_leaf();
    mlspp::KeyPackage kp(
      s->suite, s->init_priv.public_key, leaf, mlspp::ExtensionList{}, s->sig_priv);
    s->my_kp = kp;
    auto m = mlspp::tls::marshal(kp);
    if (m.size() > cap) {
      return -1;
    }
    std::memcpy(out, m.data(), m.size());
    *len = m.size();
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] key_package: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_prepare_epoch(dave_session* s, uint64_t epoch)
{
  if (!s) {
    return -1;
  }
  if (epoch != 1) {
    return 0;  // nothing to build for epochs after 1
  }
  try {
    s->current_state.reset();
    s->clear_pending();
    s->want_epoch1 = true;
    if (s->ext_sender) {
      s->create_pending_group();
    }
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] create group: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_on_welcome(dave_session* s, const uint8_t* data, size_t len,
                const char* const* recognized, int nrecognized)
{
  if (!s || !data || !s->my_kp) {
    return -1;
  }
  (void)recognized;
  (void)nrecognized;  // recognized[] is not validated
  try {
    auto welcome = mlspp::tls::get<mlspp::Welcome>(to_bytes(data, len));
    mlspp::State st(s->init_priv,
                    s->leaf_priv,
                    s->sig_priv,
                    *s->my_kp,
                    welcome,
                    std::nullopt,  // ratchet tree is inline in the Welcome
                    {});  // no PSKs
    s->current_state = std::move(st);
    s->clear_pending();
    s->my_kp.reset();
    s->want_epoch1 = false;
    console_dbgf("[dave] joined MLS group via welcome, epoch=%llu",
                 (unsigned long long)s->current_state->epoch());
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] welcome: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_on_commit(dave_session* s, const uint8_t* data, size_t len)
{
  if (!s || !data) {
    return -1;
  }
  std::optional<mlspp::State>* base_opt =
    s->state_with_proposals ? &s->state_with_proposals
                            : (s->current_state ? &s->current_state : nullptr);
  if (!base_opt || !*base_opt) {
    return -1;
  }
  try {
    auto raw = to_bytes(data, len);
    auto msg = mlspp::tls::get<mlspp::MLSMessage>(raw);

    std::optional<mlspp::State> cached;
    for (auto it = s->outbound_commits.begin(); it != s->outbound_commits.end(); ++it) {
      if (it->raw_commit == raw) {
        cached = std::move(it->next_state);
        break;
      }
    }
    if (!s->current_state && !cached) {
      return -1;
    }

    auto next = (*base_opt)->handle(msg, std::move(cached));
    if (!next) {
      return -1;
    }
    s->current_state = std::move(*next);
    s->clear_pending();
    s->my_kp.reset();
    s->want_epoch1 = false;
    console_dbgf("[dave] processed commit, epoch=%llu",
                 (unsigned long long)s->current_state->epoch());
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] commit: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_on_proposals(dave_session* s, const uint8_t* data, size_t len,
                  const char* const* recognized, int nrecognized, uint8_t* out,
                  size_t* out_len, size_t cap, uint8_t* wout, size_t* wout_len,
                  size_t wcap)
{
  if (out_len) *out_len = 0;
  if (wout_len) *wout_len = 0;
  if (!s || (!s->pending_group && !s->current_state) || !data || len < 1 ||
      !out || !out_len || !wout || !wout_len) {
    return -1;
  }
  (void)recognized;
  (void)nrecognized;  // recognized[] is not validated
  try {
    const mlspp::State& base = s->current_state ? *s->current_state : *s->pending_group;

    mlspp::tls::istream in(std::vector<uint8_t>(data, data + len));
    uint8_t optype = 0;
    in >> optype;
    if (optype != 0 && optype != 1) {
      console_logf("[dave] proposals: unexpected optype %u", (unsigned)optype);
      return -1;
    }

    auto next_queue = s->proposal_queue;
    std::optional<mlspp::State> next_state;

    if (optype == 1) {
      // Revoke proposal refs: rebuild state from base and replay surviving queued proposals.
      std::vector<bytes> refs;
      in >> refs;
      next_state.emplace(base);
      size_t revoked = 0;
      for (auto it = next_queue.begin(); it != next_queue.end();) {
        bool match = false;
        for (const auto& r : refs) {
          if (it->ref == r) { match = true; break; }
        }
        if (match) {
          revoked++;
          it = next_queue.erase(it);
        } else {
          next_state->handle(it->content);
          ++it;
        }
      }
      if (revoked != refs.size()) {
        console_logf("[dave] proposals: revoke ref mismatch (%u/%u)",
                     (unsigned)revoked, (unsigned)refs.size());
        return -1;
      }
    } else {
      // Append proposals (MLSMessage vector).
      std::vector<mlspp::MLSMessage> msgs;
      in >> msgs;
      next_state.emplace(s->state_with_proposals ? *s->state_with_proposals : base);
      for (const auto& m : msgs) {
        auto val = next_state->unwrap(m);
        auto ref = s->suite.ref(val.authenticated_content());
        for (const auto& q : next_queue) {
          if (q.ref == ref) {
            console_logf("[dave] proposals: duplicate ref");
            return -1;
          }
        }
        next_state->handle(val);
        next_queue.push_back({ std::move(val), std::move(ref) });
      }
    }

    if (next_queue.empty()) {
      s->state_with_proposals = std::move(next_state);
      s->proposal_queue = std::move(next_queue);
      *out_len = 0;
      *wout_len = 0;
      return 0;
    }

    // inline_tree=true so the emitted Welcome carries the ratchet tree; the join
    // path (dave_on_welcome) reads it with tree=nullopt, so both must agree.
    mlspp::CommitOpts opts;
    opts.inline_tree = true;
    auto [commit_msg, welcome_msg, new_state] =
      next_state->commit(mlspp::hpke::random_bytes(s->suite.secret_size()), opts, {});
    auto mc = mlspp::tls::marshal(commit_msg);
    if (mc.size() > cap) {
      return -1;
    }
    bytes mw;
    if (!welcome_msg.secrets.empty()) {
      mw = mlspp::tls::marshal(welcome_msg);
      if (mw.size() > wcap) {
        return -1;
      }
    }

    s->state_with_proposals = std::move(next_state);
    s->proposal_queue = std::move(next_queue);
    if (s->outbound_commits.size() >= kMaxOutboundCommits) {
      s->outbound_commits.pop_front();
    }
    s->outbound_commits.push_back({ mc, std::move(new_state) });

    std::memcpy(out, mc.data(), mc.size());
    *out_len = mc.size();
    if (!mw.empty()) {
      std::memcpy(wout, mw.data(), mw.size());
      *wout_len = mw.size();
    } else {
      *wout_len = 0;
    }
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] proposals: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

extern "C" int
dave_sender_key(dave_session* s, const char* sender_user_id, uint8_t generation,
                uint8_t out_key[16])
{
  if (!s || !s->current_state || !sender_user_id) {
    return -1;
  }
  try {
    uint64_t uid = parse_uid(sender_user_id);
    // base = MLS-Exporter("Discord Secure Frames v0", LE64(user_id), 16); the
    // exporter hashes the context internally, so pass the raw 8 bytes.
    auto base = s->current_state->do_export("Discord Secure Frames v0", le64(uid), 16);
    mlspp::HashRatchet ratchet(s->suite, base);
    auto kn = ratchet.get(generation);
    if (kn.key.size() < 16) {
      return -1;
    }
    std::memcpy(out_key, kn.key.data(), 16);
    return 0;
  } catch (const std::exception& e) {
    console_logf("[dave] sender_key: %s", e.what());
    return -1;
  } catch (...) {
    return -1;
  }
}

static int
selftest_mls_protocol()
{
  const uint64_t gid = 99887766554433ULL;
  auto suite = mlspp::CipherSuite(mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256);
  auto ext_priv = mlspp::SignaturePrivateKey::generate(suite);
  mlspp::ExternalSender ext{ ext_priv.public_key, mlspp::Credential::basic({ 0, 1, 1, 0 }) };
  auto ext_bytes = mlspp::tls::marshal(ext);

  dave_session* a = dave_session_create("11111111111111111", gid);
  dave_session* b = dave_session_create("22222222222222222", gid);
  if (!a || !b) {
    dave_session_free(a);
    dave_session_free(b);
    return 1;
  }

  int fail = 0;
  // Out-of-order test: prepare_epoch(1) before external_sender for A, reverse for B.
  if (dave_prepare_epoch(a, 1) != 0) fail |= 2;
  if (dave_on_external_sender(a, ext_bytes.data(), ext_bytes.size()) != 0) fail |= 2;
  if (dave_on_external_sender(b, ext_bytes.data(), ext_bytes.size()) != 0) fail |= 2;
  if (dave_prepare_epoch(b, 1) != 0) fail |= 2;

  uint8_t kp_b[8192];
  size_t kp_b_len = 0;
  if (dave_make_key_package(b, kp_b, &kp_b_len, sizeof(kp_b)) != 0) fail |= 4;

  auto kp_obj = mlspp::tls::get<mlspp::KeyPackage>(to_bytes(kp_b, kp_b_len));
  auto add_prop = mlspp::Proposal{ mlspp::Add{ kp_obj } };
  auto add_msg = mlspp::external_proposal(
    suite, be64(gid), 0, add_prop, 0, ext_priv);

  // Build op27 append payload: [uint8 optype=0][MLSMessage vector]
  mlspp::tls::ostream append_os;
  append_os << uint8_t(0) << std::vector<mlspp::MLSMessage>{ add_msg };
  auto append_bytes = append_os.bytes();

  // Build op27 revoke payload for the same proposal, verify revoke leaves 0 commits.
  auto val_ref = suite.ref(
    mlspp::tls::var::get<mlspp::PublicMessage>(add_msg.message).authenticated_content());
  mlspp::tls::ostream revoke_os;
  revoke_os << uint8_t(1) << std::vector<bytes>{ val_ref };
  auto revoke_bytes = revoke_os.bytes();

  std::vector<uint8_t> cbuf(32768), wbuf(32768);
  size_t cl = 0, wl = 0;
  if (dave_on_proposals(a, append_bytes.data(), append_bytes.size(), nullptr, 0,
                        cbuf.data(), &cl, cbuf.size(), wbuf.data(), &wl, wbuf.size()) != 0 ||
      cl == 0 || wl == 0) {
    fail |= 8;
  }
  size_t rcl = 99, rwl = 99;
  if (dave_on_proposals(a, revoke_bytes.data(), revoke_bytes.size(), nullptr, 0,
                        cbuf.data(), &rcl, cbuf.size(), wbuf.data(), &rwl, wbuf.size()) != 0 ||
      rcl != 0 || rwl != 0) {
    fail |= 16;
  }
  // Re-append and commit cleanly.
  if (dave_on_proposals(a, append_bytes.data(), append_bytes.size(), nullptr, 0,
                        cbuf.data(), &cl, cbuf.size(), wbuf.data(), &wl, wbuf.size()) != 0 ||
      cl == 0 || wl == 0) {
    fail |= 32;
  }

  if (dave_on_commit(a, cbuf.data(), cl) != 0) fail |= 64;
  if (dave_on_welcome(b, wbuf.data(), wl, nullptr, 0) != 0) fail |= 128;

  uint8_t ka1[16], kb1[16], ka2[16], kb2[16];
  if (dave_sender_key(a, "11111111111111111", 0, ka1) != 0 ||
      dave_sender_key(b, "11111111111111111", 0, kb1) != 0 ||
      std::memcmp(ka1, kb1, 16) != 0) {
    fail |= 256;
  }
  if (dave_sender_key(a, "22222222222222222", 0, ka2) != 0 ||
      dave_sender_key(b, "22222222222222222", 0, kb2) != 0 ||
      std::memcmp(ka2, kb2, 16) != 0) {
    fail |= 512;
  }

  dave_session_free(a);
  dave_session_free(b);
  return fail;
}
