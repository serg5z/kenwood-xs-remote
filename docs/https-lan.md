# LAN-only HTTPS and authentication setup

This is a **configuration template**, not an installed service. The intended route is iPhone Safari → nginx HTTPS and Basic Auth → `xs8_gateway.py` on `127.0.0.1:8080` → local XS8 Unix socket. The gateway remains read-only; every POST still returns HTTP 501. Do not expose this site to the public internet or configure router port forwarding.

## 1. Choose the LAN name and address

Use a stable Pi LAN IPv4 address (such as a DHCP reservation) and a name that the iPhone resolves on home Wi-Fi. `woodland-pi.local` is an example **only if it resolves on the iPhone**; a router-provided local DNS name is also suitable. The name typed in Safari must exactly match the certificate's DNS Subject Alternative Name (SAN). Use `https://NAME/`, not a bare IP URL, unless a separate IP SAN is issued. Check name resolution before making a certificate.

The nginx template is [deploy/nginx/kenwood-xs-remote.conf.example](../deploy/nginx/kenwood-xs-remote.conf.example). Replace every placeholder with the actual values:

| Placeholder | Example | Meaning |
| --- | --- | --- |
| `__LAN_IPV4__` | `192.168.1.20` | Pi's specific LAN IPv4 address; never `0.0.0.0`. |
| `__LAN_HOSTNAME__` | `woodland-pi.local` | Exact Safari URL and certificate DNS SAN. |
| `__SERVER_CERT_PATH__` | `/etc/nginx/kenwood-xs-remote/server.crt` | Signed server certificate on the Pi. |
| `__SERVER_KEY_PATH__` | `/etc/nginx/kenwood-xs-remote/server.key` | Server private key on the Pi. |
| `__PASSWORD_FILE_PATH__` | `/etc/nginx/kenwood-xs-remote/users.htpasswd` | Basic Auth password hashes on the Pi. |

The template has **no port 80 listener**. Check all other enabled nginx sites as well; remove any default or duplicate listener that would expose this application through unencrypted HTTP or bypass authentication. Its `auth_basic` directives cover the whole UI and API, including future paths. The template strips the browser's `Authorization` header before proxying to the gateway. nginx's TLS, authentication, and proxy directives are documented by [nginx](https://nginx.org/en/docs/http/ngx_http_ssl_module.html), [nginx Basic Auth](https://nginx.org/en/docs/http/ngx_http_auth_basic_module.html), and [nginx proxy](https://nginx.org/en/docs/http/ngx_http_proxy_module.html).

## 2. Make a private CA and server certificate on an administrator computer

The following is an example for an administrator computer with OpenSSL. Work in a private directory with restrictive permissions. **Never copy `ca.key` or `ca.srl` to the Raspberry Pi.** Back up the encrypted CA key securely off the Pi; losing it prevents certificate renewal without re-pairing phones. Use the actual LAN hostname in place of the example.

```sh
umask 077
LAN_HOSTNAME=woodland-pi.local
openssl genpkey -algorithm RSA -aes-256-cbc -pkeyopt rsa_keygen_bits:3072 -out ca.key
cat > ca.cnf <<'EOF'
[req]
distinguished_name = dn
x509_extensions = v3_ca
prompt = no
[dn]
CN = Kenwood XS Remote Local CA
[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
EOF
openssl req -new -x509 -sha256 -days 3650 -key ca.key -out ca.crt -config ca.cnf
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out server.key
openssl req -new -sha256 -key server.key -out server.csr -subj "/CN=${LAN_HOSTNAME}"
cat > server.ext <<EOF
[server]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:${LAN_HOSTNAME}
EOF
openssl x509 -req -sha256 -days 365 -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -extfile server.ext -extensions server -out server.crt
openssl verify -purpose sslserver -CAfile ca.crt server.crt
openssl x509 -in server.crt -noout -text
openssl x509 -in ca.crt -noout -fingerprint -sha256
```

Confirm the final output contains the intended DNS SAN, `serverAuth`, and validity dates. The 2048-bit RSA server key, SHA-256 signature, DNS SAN, `serverAuth` EKU, and 365-day validity meet Apple's published TLS certificate requirements. Renew the server certificate before expiry; keep the same trusted CA until deliberately rotating it. [Apple certificate requirements](https://support.apple.com/en-gb/103769), [OpenSSL certificate options](https://docs.openssl.org/3.6/man1/openssl-x509/).

Transfer only `server.crt` and `server.key` to the Pi over an authenticated administrative channel. Keep `server.key` readable only by root (`root:root`, mode `0600`) and `server.crt` non-secret (for example mode `0644`). Never store the server key, CA key, password file, or real certificates in this repository. Verify the installed nginx master process can read the server key without making it world-readable.

## 3. Create the Basic Auth account on the Pi

With nginx and a password-file utility available, create a unique password interactively; do not put it on a command line or in a URL. For example, `htpasswd` from `apache2-utils` supports bcrypt with `-B`:

```sh
sudo install -d -o root -g www-data -m 0750 /etc/nginx/kenwood-xs-remote
sudo htpasswd -cB /etc/nginx/kenwood-xs-remote/users.htpasswd remote
sudo chown root:www-data /etc/nginx/kenwood-xs-remote/users.htpasswd
sudo chmod 0640 /etc/nginx/kenwood-xs-remote/users.htpasswd
```

Protect the containing directory from other local users while allowing nginx to traverse it. Do not repeat `-c` when adding another user; it replaces the file. Confirm the nginx worker can read the hash file. Basic Auth credentials are reusable, so **only send them over validated HTTPS**. The password prompt may recur in Safari or a Home Screen web app; one-tap launch does not promise permanent login. [nginx password-file format](https://nginx.org/en/docs/http/ngx_http_auth_basic_module.html).

## 4. Trust the CA on the iPhone

Transfer only the public `ca.crt` to the iPhone through a trusted channel. Verify its fingerprint against the administrator computer before installation. Install the certificate profile, then go to **Settings → General → About → Certificate Trust Settings** and enable full trust for this root CA. A manually installed profile is not automatically trusted for TLS. Open `https://woodland-pi.local/` (or the exact chosen name) in Safari; do not bypass a certificate warning. [Apple trust instructions](https://support.apple.com/en-us/102390).

Trusting this CA allows it to sign certificates for other names too. Protect its private key accordingly, and remove the CA from the iPhone if the installation is retired or the key is compromised.

## 5. Validate before enabling LAN access

Install the filled nginx template as a site only after checking the certificate and password-file paths. Keep the gateway on loopback using `python3 xs8_gateway.py` with **no** `--bind` or `--allow-insecure-readonly-lan`. The gateway user must still have permission to open the local XS8 socket. On the Pi, after nginx is installed and configured, check its configuration and listeners:

```sh
sudo nginx -t
sudo nginx -T
ss -ltnp
```

On the administrator computer holding the public `ca.crt`, test the complete LAN route (replace the example name if needed):

```sh
curl --cacert ca.crt -i https://woodland-pi.local/api/v1/capabilities
curl --cacert ca.crt -u remote -i https://woodland-pi.local/api/v1/capabilities
curl --cacert ca.crt -u remote -X POST -i https://woodland-pi.local/api/v1/mute
```

Review `nginx -T` for a single intended HTTPS site on the specific LAN IPv4 address, no HTTP route to the gateway, and no unauthenticated alternate site. `ss` should show the gateway only on `127.0.0.1:8080` and nginx on the selected LAN IPv4 address port 443. The first `curl` should get HTTP 401, the authenticated GET should succeed, and the authenticated POST should remain HTTP 501. `curl -u remote` prompts for the password; do not place it in shell history. Test the same outcomes in iPhone Safari. The gateway's status endpoint can still return 503 if the XS8 service is down; that is not an authentication failure. Keep the router free of port forwarding and check that no IPv6 or other interface exposes the site unexpectedly.

## Disable the temporary HTTP preview and rollback

Before enabling nginx, stop the temporary gateway process started with `--bind LAN_IP --allow-insecure-readonly-lan`. Start it again with defaults so it binds to `127.0.0.1:8080`; confirm with `ss -ltnp` that no LAN port 8080 remains. The nginx template intentionally does not redirect port 80, because there is no authenticated HTTP listener to redirect from.

To roll back, disable only the nginx site and keep (or restart) the gateway on loopback. The established Mac workflow remains available through an SSH tunnel:

```sh
ssh -N -L 8080:127.0.0.1:8080 pi@woodland-pi.local
```

Then open `http://127.0.0.1:8080/` on the Mac. Adjust the SSH account or hostname to match the Pi. Do not roll back to the unauthenticated LAN HTTP preview for routine use. This procedure does not change `xs8-service`, its Unix socket, CEC, GPIO, or motor-control behavior.

**Future controls:** Basic Auth restricts access but does not prevent cross-site request forgery. Before enabling any state-changing endpoint, add origin checks and CSRF protection in the gateway, validate requests, and keep motor steps bounded inside the single-owner XS8 service. [OWASP CSRF guidance](https://cheatsheetseries.owasp.org/cheatsheets/Cross-Site_Request_Forgery_Prevention_Cheat_Sheet.html).
