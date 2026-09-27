# Security

## Security model

The module was designed for a closed installation, such as a boat's own network, where every device on the network and
on the wires is trusted. Know these properties before you deploy it anywhere else.

| Property | Consequence |
|---|---|
| **Kogger SBP has no authentication.** The "key" `0xC96B5D4A` in dangerous settings is a public constant that guards against accidental writes, not against attackers | anyone who can send frames to the module can change its settings |
| **The module accepts control frames from the network** on the port of every open UART line ([docs/NETWORK.md](docs/NETWORK.md) §5) | any client of the Wi‑Fi network can reconfigure the module, reboot it, change its role, and talk to the devices on its UART lines |
| **Firmware images are not signed** | the module accepts any correctly built image of this project |
| **Updates are accepted only over a wired line** (UART or native USB, since 0.11) | a network client cannot replace the firmware; a person with physical access can |
| **The default access‑point password `kogger1234` is public** (it is in this repository) | an access point left on defaults is open to anyone in radio range |
| Saved Wi‑Fi passwords are never sent out, over SBP or the text protocol | they can still be read from flash by someone with physical access (no flash encryption) |

## Before deployment

1. **Change the access‑point password** before or right after switching to the AP role (`ID_WIFI_NET` v1). Use WPA2 or
   WPA2/WPA3 with a long random password. Station is the default role, so a module out of the box runs no access
   point.
2. Keep the module on a network where every client is trusted, or keep its lines off (mode "off") when it is not relaying.
3. For protection against foreign firmware, enable signed images (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA‑3072)
   and keep the signing key off the build machine. See [docs/UPDATE.md](docs/UPDATE.md).
4. For protection of saved passwords against physical access, consider ESP‑IDF flash encryption. This project does not
   use or test it.

## Reporting a vulnerability

Please report security issues privately through GitHub's "Report a vulnerability" (Security → Advisories) rather than in
a public issue. If that option is not shown, contact KOGGER LLC through <https://kogger.tech> and ask for a private
channel before sending details.
