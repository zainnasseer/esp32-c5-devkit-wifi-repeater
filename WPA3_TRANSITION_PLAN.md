# WPA3 Upgrade Plan — enable WPA3-Personal while keeping full compatibility

**Goal:** add WPA3-Personal (SAE) so capable devices get its stronger security, **without** locking out
WPA2-only clients or WPA2-only routers. The mechanism is **WPA2/WPA3 transition mode** on the
repeater's AP and a **WPA2-minimum threshold** on the STA uplink.

> Companion to `WIFI_PROVISIONING_PLAN.md`. All changes here are config + a few struct fields; it is
> a low-risk, reversible upgrade.

---

## §0 — Current State (from your build)

| Item | Today | Source |
|------|-------|--------|
| WPA3/SAE in firmware | **disabled** (`CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n`) | `sdkconfig.defaults:97` |
| AP auth | `WIFI_AUTH_WPA_WPA2_PSK` (→ OPEN if password < 8) | `main.c:420`, `:430-437` |
| STA auth threshold | `WIFI_AUTH_WPA_WPA2_PSK`, PMF capable / not required | `main.c:400-403` |
| Provisioning AP | OPEN when default AP password is empty/short | `main.c:431-437` |
| App size headroom | binary ≈ 905 KB in a 1 MB partition → **≈ 143 KB free** | `build/wifi_repeater.bin`, `partitions.csv` |

SAE was turned off deliberately to save flash. Re-enabling it is step 1; the only real cost is binary size.

---

## §1 — Compatibility Principle (why *transition* mode)

- **AP = `WIFI_AUTH_WPA2_WPA3_PSK` (transition).** WPA3 devices authenticate with SAE; WPA2 devices still join with PSK. WPA3-**only** (`WIFI_AUTH_WPA3_PSK`) would reject every WPA2 client — unacceptable for a repeater that must serve whatever connects.
- **PMF capable, not required.** WPA3 mandates Protected Management Frames; transition mode advertises PMF as *capable* but **not required**, so older clients with flaky PMF still associate.
- **STA threshold stays at WPA2.** Keep `threshold.authmode = WIFI_AUTH_WPA2_PSK` (minimum), so the uplink works with any router and **auto-uses WPA3 when the router offers it**. Do **not** set the threshold to `WIFI_AUTH_WPA3_PSK` — that would reject WPA2 routers.

**One deliberate compatibility change:** moving the AP from `WPA_WPA2` to `WPA2_WPA3` drops legacy **WPA1/TKIP**. That's intended — WPA1 is insecure and deprecated — but note that any ancient WPA1-only client would no longer connect. Everything WPA2 and newer is unaffected.

---

## §2 — Changes by File

### 2.1 `sdkconfig.defaults` — enable SAE + confirm crypto

```diff
- CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n
+ CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=y
```

Also, in `menuconfig → Component config → Wi-Fi`, enable **SoftAP SAE support** (the AP-side WPA3
option — confirm the exact symbol in your IDF, e.g. `CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT`). The STA-side
flag above is not enough for the AP to *offer* WPA3.

Because you trimmed mbedtls, **verify SAE's crypto is present** (SAE group 19 uses NIST P-256):

```
CONFIG_MBEDTLS_ECP_C=y
CONFIG_MBEDTLS_ECDH_C=y
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y
```

If any were trimmed off, SAE will fail to negotiate — re-enable them.

### 2.2 `main.c` — AP in transition mode (`~line 413-420`)

```c
wifi_config_t ap_config = {
    .ap = {
        .ssid = "", .ssid_len = 0, .channel = 1, .password = "",
        .max_connection = 0,
        .authmode    = WIFI_AUTH_WPA2_WPA3_PSK,        // was WIFI_AUTH_WPA_WPA2_PSK
        .pmf_cfg     = { .capable = true, .required = false },
        .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,              // H2E + hunt-and-peck (max client compat)
    },
};
```

Keep your existing **"< 8 chars → OPEN"** fallback (`main.c:430-437`) — WPA3 also requires a passphrase
of at least 8 characters, so the guard is still correct. (Optional: instead of silently going OPEN on a
short password, reject it so the AP is never unintentionally open.)

### 2.3 `main.c` — STA accepts WPA3 when offered (`~line 398-403`)

```c
.threshold.authmode = WIFI_AUTH_WPA2_PSK,   // minimum WPA2 (works with any router; auto-uses WPA3)
.pmf_cfg            = { .capable = true, .required = false },
.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH,     // allow SAE when the upstream router is WPA3
```

> Note: you currently allow `WPA_WPA2` (includes WPA1) on the STA. Raising the minimum to `WPA2_PSK`
> is a small security win and only excludes ancient WPA1-only routers. Keep `WPA_WPA2` if you need that
> last bit of router compatibility — either way, SAE/WPA3 is negotiated when available.

### 2.4 (Optional) Secure the provisioning AP with WPA3 transition

The provisioning AP is currently OPEN (no default AP password), so the upstream password the user types
during setup crosses the air in clear. To close that, ship a **default AP password ≥ 8 chars**; the AP
then comes up WPA2/WPA3 transition automatically via the change in §2.2 — no extra code, the setup
credentials are now encrypted over the air. (Trade-off: users must know the default AP password; print
it on a label or derive it from the MAC.)

---

## §3 — Compatibility Matrix (expected result)

| Client → / Router ↓ | WPA3 client | WPA2 client | WPA1-only client |
|---------------------|-------------|-------------|------------------|
| **WPA3 router**     | SAE end-to-end | AP serves WPA2 (transition); uplink WPA3 | rejected by AP |
| **WPA2 router**     | AP→client WPA3 (SAE); uplink WPA2 | WPA2 throughout | rejected by AP |
| **WPA1-only router**| uplink only if STA threshold left at `WPA_WPA2` | same | n/a |

Net: every WPA2-and-newer device and router keeps working; WPA3 is used opportunistically on each
hop. Only deprecated WPA1/TKIP is dropped on the AP.

---

## §4 — Flash Budget

- Current app: **≈ 905 KB**; partition: **1 MB** → **≈ 143 KB free**.
- WPA3-SAE typically adds **~30–50 KB** (supplicant SAE + ECC). Should fit comfortably, but **verify**:

```bash
idf.py build
idf.py size            # confirm app still fits the 1 MB factory partition
idf.py size-components # see how much SAE/mbedtls added, if you need to trim elsewhere
```

If it ever gets tight, reclaim space by trimming unused mbedtls ciphers or log verbosity — don't shrink
the `www` SPIFFS partition.

---

## §5 — Test Plan

```bash
idf.py build && idf.py size && idf.py flash monitor
```

1. **WPA3 client:** a modern phone connects to the AP; confirm it negotiates **SAE/WPA3** (phone shows WPA3, or check supplicant logs).
2. **WPA2 client:** an older laptop/IoT device still connects to the same AP (transition mode works).
3. **Uplink — WPA2 router:** STA connects normally.
4. **Uplink — WPA3 router:** STA connects and negotiates SAE.
5. **PMF sanity:** try 3–4 mixed devices; confirm no association failures (PMF not required).
6. **Provisioning (if §2.4 applied):** provisioning AP is now WPA2/WPA3, setup credentials encrypted over the air.
7. **Size:** `idf.py size` confirms the app still fits the 1 MB partition.

---

## §6 — Rollback

Everything is config: set `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n` and revert the two `authmode` fields to
`WIFI_AUTH_WPA_WPA2_PSK` to return to the current WPA2 behavior. No data or partition changes involved.

---

## §7 — File Change Summary

| File | Action | Change |
|------|--------|--------|
| `sdkconfig.defaults` | MODIFY | `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=y`; confirm SoftAP SAE + mbedtls ECP/ECDH/P-256 |
| `main.c` (AP cfg) | MODIFY | `authmode = WIFI_AUTH_WPA2_WPA3_PSK`; add `pmf_cfg` + `sae_pwe_h2e` |
| `main.c` (STA cfg) | MODIFY | add `sae_pwe_h2e`; keep WPA2 threshold + PMF capable/not-required |
| `main.c` (provisioning AP) | OPTIONAL | default AP password ≥ 8 chars to make provisioning AP WPA2/WPA3 instead of OPEN |

**Defense one-liner:** *"WPA3 is enabled in WPA2/WPA3 transition mode with PMF capable-but-not-required,
so SAE is used by capable devices on each hop while every WPA2 client and router keeps working — the
only cost is ~40 KB of flash, which fits the existing partition."*
