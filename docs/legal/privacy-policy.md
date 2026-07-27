# CombatForge — Privacy Policy

**Last updated: «DATE»**
**Effective: «DATE»**

> ⚠️ **DRAFT — needs a lawyer before you publish it.** This is written to be *accurate to what
> CombatForge's code actually does* (I traced the data flows in `PFBackendSubsystem.cpp` and the
> `combatforge-api` endpoints), which is the hard part and the part a template gets wrong. But a
> published privacy policy is a binding legal representation, and GDPR/CCPA/COPPA exposure is real.
> Have counsel review it. Placeholders in «guillemets» must be filled in.

---

## Who we are

CombatForge is a video game developed and operated by «LEGAL ENTITY — e.g. "Tom Chapman", sole
proprietor, or an LLC if you form one». In this policy, "we", "us" and "our" mean that operator, and
"the Game" means CombatForge and its related online services at `playcombatforge.com`.

**Contact:** «PRIVACY CONTACT EMAIL — e.g. privacy@playcombatforge.com»

> You do not currently have a contact address configured anywhere in the project. Stores require
> one, and so does GDPR. Set up a real mailbox before publishing.

## The short version

CombatForge is a multiplayer shooter. To let you play online, keep your progress, and match you with
other players, we store a small amount of data about your account and your matches. **We do not sell
your data, we do not run advertising, and we do not embed third-party analytics or tracking SDKs.**

## What we collect

### Account information

When you create a CombatForge account, you sign in through a browser using an OAuth 2.0 device-code
flow. **The Game client never sees or stores your password.** From that process we hold:

| Data | Why |
|---|---|
| Display name | Shown to other players in-game, on scoreboards and match results |
| Account identifier and session token | To keep you signed in and tie progress to your account |
| Email address (if provided to our identity provider) | Account recovery and service notices |

An account is optional for local and LAN play. It is required for online progression, class
unlocks, and playing on our official servers.

### Installation identifier

The Game generates a **pseudonymous hashed installation identifier** (`guidHash`). It is not derived
from your name, email, or hardware serial numbers. We use it to link a local profile to an account,
to de-duplicate map votes, and to preserve match statistics if you disconnect mid-game.

### Gameplay and progression data

When you finish a match on an official server, the server reports:

- Match identifier, game mode, map, and match duration
- Your per-match statistics: eliminations, tags, objective score, pieces built, whether you completed
  the match, and whether you won
- Your resulting experience, rank, and unlocked items

We use this to maintain your progression, populate scoreboards, and balance the game.

### Server browser data

Game servers publish their own status — address, port, player count, current phase, map, mode and
format — so the in-game browser can list them. This is information about *servers*, not about you
personally.

### Player-submitted content

- **Community maps.** Arenas you build can be saved as map files and shared with other players.
  These contain the layout you built and the match it came from — not personal information.
- **Bug reports.** If you use the in-game or web bug reporter, whatever you type is forwarded to a
  private channel in our Discord server, along with any technical details you include. **Do not put
  personal or sensitive information in a bug report.**

### Data stored only on your own device

Some data never leaves your computer unless you send it to us: your settings and key bindings, your
saved session token, locally saved community maps, and game log files. Log files can contain your
computer's hostname, local network address and hardware details — this is standard engine
diagnostic output. We do not collect these logs automatically; we only see them if you choose to
send one to us for support.

### What we do NOT collect

- No advertising identifiers, and no advertising
- No third-party analytics or telemetry SDKs
- No voice chat, and no microphone or camera access
- No payment card data — CombatForge is free during beta, and any future purchases would be handled
  entirely by the storefront (Epic Games Store, itch.io), not by us
- No precise location data

## How we use it

We use the data above only to: operate and secure the Game and its servers; keep you signed in;
maintain your progression and unlocks; run matchmaking, scoreboards, and the server browser; detect
and prevent cheating, abuse, and fraud; diagnose crashes and fix bugs; and communicate with you about
the service.

**We do not sell personal information, and we do not share it for cross-context behavioural
advertising.**

## Legal bases (UK/EU users)

Where GDPR applies, we rely on: **contract** — to give you an account and let you play online;
**legitimate interests** — to keep the Game secure, prevent cheating, and improve it; and
**consent** — where we ask for it, which you can withdraw at any time.

## Who we share it with

We use a small number of service providers who process data on our behalf:

| Provider | Purpose |
|---|---|
| Cloudflare | Hosting for our API, database, and website |
| Vultr | Hosting for official game servers |
| Discord | Receives bug reports you choose to submit |
| Epic Games / itch.io | Distribution platforms; they operate under their own privacy policies |

We may also disclose information where legally required, or to protect our rights, users, or the
safety of others.

**Note on multiplayer:** your display name, in-game statistics, and the maps you build are visible
to other players by design.

## International transfers

We are based in the United States and our providers may process data in the United States and
elsewhere. Where we transfer personal data out of the UK/EEA, we rely on appropriate safeguards such
as Standard Contractual Clauses.

## How long we keep it

We keep account and progression data for as long as your account is active. **If you ask us to delete
your account, we will delete or anonymise your personal data within 30 days**, except where we must
keep something to comply with the law or resolve disputes. Match records and community maps may be
retained in anonymised form, with no link back to you.

## Your rights

Depending on where you live, you may have the right to access, correct, delete, or export your
personal data; to object to or restrict certain processing; to withdraw consent; and to complain to
your data protection authority. California residents have rights under the CCPA/CPRA, including the
right to know, delete, correct, and to not be discriminated against for exercising them — note again
that **we do not sell or share personal information** as those terms are defined.

To exercise any of these, email «PRIVACY CONTACT EMAIL». We will not treat you differently for asking.

## Children

**CombatForge accounts are not intended for children under 13** (or under the minimum digital consent
age in your country, which is up to 16 in parts of the EEA). We do not knowingly collect personal
information from children under 13. If you believe a child has created an account, contact us and we
will delete it.

> ⚠️ **Tom — read this one.** The Game currently has **no age gate anywhere**. If under-13 players
> create accounts, US COPPA obligations attach (verifiable parental consent, and much stricter data
> rules) and the penalties are severe. Epic's IARC questionnaire will ask about this directly.
> Two realistic options: (a) add a date-of-birth or 13+ confirmation to account creation and block
> under-13 accounts, or (b) keep accounts 13+ by policy and enforce it at the identity provider.
> Option (a) is a small change to the account-creation flow in `combatforge-api` — say the word and
> I'll implement it. This is the single biggest legal gap in shipping as-is.

## Security

We use HTTPS for all network traffic, sign match reports with HMAC-SHA256 so scores cannot be forged,
and limit access to production systems. No system is perfectly secure, but we take this seriously.

> ⚠️ **Known issue to fix before beta:** your session token is currently written to disk as plaintext
> JSON (logged as an open finding in `docs/code-review/modules/online.md`). A packaged build run from
> a writable folder has leaked a live token before (the 2026-07-17 incident). Worth closing before a
> public launch — this policy promises reasonable security.

## Changes

If we make material changes we will update the "Last updated" date and, where required, notify you
in-game or by email. Continuing to play after a change means you accept the updated policy.

## Contact

«PRIVACY CONTACT EMAIL» — «POSTAL ADDRESS, if required by your jurisdiction or a storefront»
