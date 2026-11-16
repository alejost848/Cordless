#include "tls/ca_bundle.h"
#include "console.h"
#ifdef CORDLESS_PLUGIN
#include "discord_ca.h"
#endif

static mbedtls_x509_crt s_ca;
static int s_state;  // 0 = untried, 1 = loaded, -1 = failed

void ca_bundle_init(const char *cacert_path)
{
    if (s_state != 0) return;  // parse exactly once
    mbedtls_x509_crt_init(&s_ca);
#ifdef CORDLESS_PLUGIN
    (void)cacert_path;
    if (mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)DISCORD_CA_PEM,
                               sizeof(DISCORD_CA_PEM)) >= 0) {
        s_state = 1;
        console_logf("[tls] slim Discord CA bundle parsed (%u bytes)",
                     (unsigned)sizeof(DISCORD_CA_PEM));
    } else {
        s_state = -1;
        console_logf("[tls] slim Discord CA bundle FAILED");
    }
#else
    // parse_file returns 0 on full success or a positive count of certs that failed
    // (still usable); only a negative return is a hard failure.
    if (cacert_path && mbedtls_x509_crt_parse_file(&s_ca, cacert_path) >= 0) {
        s_state = 1;
        console_dbgf("[tls] CA bundle parsed once: %s", cacert_path);
    } else {
        s_state = -1;
        console_logf("[tls] CA bundle FAILED to load (%s); TLS will be unverified",
                     cacert_path ? cacert_path : "(null)");
    }
#endif
}

mbedtls_x509_crt *ca_bundle_get(void)
{
    return s_state == 1 ? &s_ca : NULL;
}
