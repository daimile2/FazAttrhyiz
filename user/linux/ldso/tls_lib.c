/* Original CapMix golden IE TLS module. Compile twice with distinct symbols. */
#ifndef TLS_SECOND
#define VALUE tls_a_value
#define ZERO tls_a_zero
#define SET tls_a_set
#define CHECK tls_a_check
#define INITIAL 17
#else
#define VALUE tls_b_value
#define ZERO tls_b_zero
#define SET tls_b_set
#define CHECK tls_b_check
#define INITIAL 29
#endif
__thread long VALUE __attribute__((aligned(64)))=INITIAL;
__thread long ZERO;
int SET(long cookie)
{
    if(VALUE!=INITIAL || ZERO)return 0;
    VALUE=cookie;ZERO=~cookie;return 1;
}
int CHECK(long cookie){return VALUE==cookie && ZERO==~cookie;}
