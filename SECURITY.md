# Security Policy

## Security Model

Satoshi Pet is designed with security in mind:

### What Satoshi Pet Does NOT Do

- ❌ Store Bitcoin private keys
- ❌ Have access to your Bitcoin funds
- ❌ Make any Bitcoin transactions
- ❌ Store sensitive credentials in firmware

### What Satoshi Pet Does

- ✅ Display your Ganamos balance (read-only)
- ✅ React to balance changes
- ✅ Communicate with Ganamos API over HTTPS
- ✅ Store only a device ID (not credentials)

## Authentication

The device authenticates using a unique `deviceId` (UUID) assigned by the Ganamos backend during pairing. This ID:

- Is randomly generated
- Cannot be used to access funds
- Only allows reading balance and spending "coins" (virtual pet currency)
- Is tied to a single Ganamos account

## Data Stored on Device

The device stores in flash memory:
- `deviceId` - UUID linking to Ganamos account
- `pairingCode` - Used for initial pairing only
- `petName` / `petType` - Pet customization
- `lastBalance` / `lastCoins` - Cached values for offline display
- Pet stats (hunger, happiness)

**No passwords, API keys, or sensitive data are stored.**

## Network Security

- All API communication uses HTTPS
- Certificate validation is disabled for compatibility (embedded devices)
- Rate limiting protects against abuse
- Device cannot modify Bitcoin balances

## Reporting Vulnerabilities

If you discover a security vulnerability, please:

1. **DO NOT** open a public issue
2. Email security concerns to: security@ganamos.earth
3. Include:
   - Description of the vulnerability
   - Steps to reproduce
   - Potential impact
   - Any suggested fixes

We will respond within 48 hours and work with you to address the issue.

## Responsible Disclosure

We ask that you:
- Give us reasonable time to fix issues before public disclosure
- Do not exploit vulnerabilities beyond what's needed to demonstrate them
- Do not access or modify other users' data

## Scope

This security policy covers:
- Satoshi Pet firmware (this repository)
- Communication between device and Ganamos API

For Ganamos platform security, contact security@ganamos.earth directly.

## Known Limitations

1. **No Certificate Pinning** - The device uses `setInsecure()` for SSL. This is common in embedded devices but theoretically allows MITM attacks on the local network.

2. **Physical Access** - Anyone with physical access to the device can read the deviceId from flash memory. This only allows reading balance, not accessing funds.

3. **WiFi Credentials** - WiFi passwords are stored in the ESP32's flash. Physical access allows extraction.

These are acceptable tradeoffs for a fun, educational device that has no access to actual Bitcoin funds.

## Updates

We recommend keeping your firmware up to date for the latest security improvements.

---

Thank you for helping keep Satoshi Pet secure! 🔐
