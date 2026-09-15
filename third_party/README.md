# third_party

## tls-mbedtls/

curl built against mbedTLS, vendored into the project instead of installed into
the shared VitaSDK (the SDK's installed curl uses OpenSSL 1.1.1, which is reported
to run out of pthread locks on real hardware when loading a CA bundle).

Downloaded 2026-09-15 with the SDK's pacman in download-only mode against a
temporary copy of the pacman database (SDK database and files untouched):

    pacman --config $VITASDK/etc/pacman.conf --root $VITASDK --dbpath /tmp/vr-pacdb/db \
           --logfile /tmp/vr-pacdb/pacman.log --gpgdir /tmp/vr-pacdb/gpg \
           --noconfirm -Sw --cachedir third_party/pkgcache curl-mbedtls mbedtls

Repository: `https://github.com/vitasdk/vitasdk-autobuild/releases/download/packages-2026.08-snapshot-20260912.42.1`

| Package file | sha256 |
|---|---|
| `pkgcache/curl-mbedtls-8.22.0-1-vita.pkg.tar.xz` | `249b6992000b77dc2cac59feec55bea7bb892f181c03cb9d5399504290b6d30f` |
| `pkgcache/mbedtls-3.6.5-1-vita.pkg.tar.xz` | `f0340ba0eae36b2e8358ec033245e85ad442395fcffe3cf19dcc1a9551cfcf07` |

Both archives were extracted with `tar -xJf`; their `arm-vita-eabi/include` and
`arm-vita-eabi/lib` trees were copied into `tls-mbedtls/`:

- `include/curl`, `include/mbedtls`, `include/psa`, `include/everest`
- `lib/libcurl.a`, `lib/libmbedtls.a`, `lib/libmbedx509.a`, `lib/libmbedcrypto.a`
  (plus `libeverest.a`, `libp256m.a`, pkgconfig and cmake files)

The Makefile uses these automatically when `tls-mbedtls/lib/libcurl.a` exists
(`TLS=mbedtls`); otherwise it falls back to the SDK's curl + OpenSSL (`TLS=openssl`).
