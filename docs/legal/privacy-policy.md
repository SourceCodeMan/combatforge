# CombatForge Privacy Policy — canonical source

The release-candidate policy is maintained as the page that will actually be deployed:

- Source: `combatforge-site/public/privacy.html`
- Public URL: <https://playcombatforge.com/privacy.html>
- Effective/updated date in that source: August 24, 2026

Do not publish a second copy from this repository; duplicated legal text drifts from the deployed
service. The canonical page was reconciled against the alpha.20 release-hardening code and states:

- Accounts are 13+ only; signup evaluates a birth date but does not store it.
- Direct Better Auth signup bypasses and legacy under-13 sessions are blocked.
- The service creates a non-routable placeholder email and does not request a real email at launch.
- Match/progression, install hash, server directory, and account/session processing are disclosed.
- Bug reports no longer collect contact data or retain the request IP hash/User-Agent with a report;
  a salted IP hash exists only in short-lived abuse-control buckets.
- The marketing site has no analytics, third-party fonts, scripts, or embedded video. YouTube is an
  external link that is contacted only when clicked.
- The Windows client protects the cached bearer token with DPAPI.

Owner gates before deployment:

- Confirm that **Tom Chapman** is the correct legal operator name for the store organization.
- Confirm `hello@playcombatforge.com` is monitored and appropriate for privacy requests.
- Obtain legal review for every country/region selected in Epic Developer Portal.
