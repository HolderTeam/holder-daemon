# Privacy Model

This document describes Holder's privacy model for card content.

## Philosophy

Git is a very powerful tool. However, Uncle Ben Parker from *Spider-Man*
(distilling the words of Jesus from Luke 12:48) said:

> “With great power comes great responsibility.”

Highly technical users who are fluent with Git can use plain Git
repositories for their projects, so they have an escape hatch to use tools
like `grep`, `sed` and `vim` on their cards when they want.

They can use Holder and a remote Git server to collaborate with other people.
They can set up private or public remotes and know not to put sensitive data,
such as passwords, into them. And yet even experienced developers sometimes
commit secrets by mistake.

However, Holder is for everyone. Giving the awesome power of Git to less
technical users can completely transform their ability to track their work
and collaborate.

But what if they put private data into a card, mistakenly set up a public
rather than private repository, and then push it, publishing it to the web?

So spreading the awesome power of Git to non-technical users needs to come
with a guardrail.

The default approach is to encrypt the card body using the XChaCha20-Poly1305
algorithm (via libsodium) in an attempt to reduce the power of the privacy
footgun.

The user keeps the ability to back up their cards and sync them between
devices. This doesn’t affect the features for working with cards in Holder,
but it does remove the immediate escape hatch of using plain-text tools
directly on the card files. It also means Git itself is no longer the
collaboration medium in the traditional sense.

However, cards can always be exported or copied into non-encrypted projects.
And as the user grows in confidence and competence, they can make plain
projects if they want.

For completeness, I did try using Git filters to allow cards to remain
plaintext in the working tree and then be encrypted in the index using tools
such as `age` or `git-crypt`.

The idea is that you “clean” (encrypt) the card when you stage it,
and “smudge” (decrypt) the card when Git writes it back to the working tree.
I wrote a small command-line tool called `holder-crypt`,
which Git shelled out to in order to perform the encryption and decryption.

But in testing, I found this approach fragile and full of new footguns.
If anything went wrong: a missing filter, misconfiguration, or tooling issue;
Git could silently commit data without filtering it,
leaving it in plaintext in the repository history.

And it doesn’t really buy as much as you might think. No major Git hosting
provider (GitHub, GitLab, Bitbucket) decrypts SOPS, age, or git-crypt
content server-side for viewing. So you still lose one of the main benefits
of remote Git servers: the ability to collaborate directly through the
hosting platform’s interface.

So the current approach avoids Git clean/smudge filters because
misconfiguration or missing tooling can silently commit plaintext.
Encrypting the working tree makes Git a simple transport and prevents
accidental leakage by design.

It is also significantly more performant to use libsodium to encrypt the
cards directly in C++ as they are written, rather than relying on
a chain of shell tools at commit time.

And although I built encrypted cards to try to reduce the risk
of non-technical users exposing their own secrets, I’ve found
I rather like it myself.

I can write freely without worrying about privacy in the moment.
If something is worth sharing, I can tidy it up and move it into a
plain project.

Private by default. Public by choice. That feels like the right way round.

Encryption by default aligns with the Holder philosophy: you can scrawl ideas
without care and refine them later, getting them down without friction. Once a thought is safely captured, it stops demanding attention and your mind can flow onto the next thing, or even to the relaxed state of an empty mind. The freedom to get a thought out of your head, without hesitation, feels liberating. 

The flow is a release.


## Scope

- Holder supports two project modes:
  - `plain`: normal markdown card blobs in Git.
  - `encrypted_git`: card blobs are envelope-encrypted at rest.
- On first run, Holder creates a default `Home` project in `encrypted_git` mode.
- Full-disk/device security is out of scope; this feature targets repository/history privacy.

## Current Backend Design

- Crypto backend: libsodium `XChaCha20-Poly1305` AEAD.
- Each encrypted project gets a random 32-byte project key.
- Project key material is stored in OS keyring (`libsecret`) or test keystore override.
- DB stores key metadata (`project_key_id`), never raw secret bytes.

## Envelope Format

- Encrypted card blobs are stored as a text envelope:
  - Header line: `HolderPriv1`
  - Metadata line: JSON (`key_id`, per-message value, etc.)
  - Ciphertext line: base64
- Holder can detect encrypted blobs by envelope header and decrypt in-app.

## Write/Read Behavior

- In `encrypted_git` projects:
  - `CardStore` encrypts content before writing card blobs.
  - API read paths decrypt and return plaintext content to clients.
- In `plain` projects:
  - card blobs are written/read as plaintext markdown.

## Commit Safety Guard

- For `encrypted_git` projects, Holder validates staged `cards/**` blobs before commit.
- If staged blobs are plaintext (missing envelope header), the operation fails with typed privacy errors.

## Recovery Token

- Recovery token export/import is supported via API:
  - export wraps project key material under user PIN-derived wrapping key.
  - import validates token/project and restores key material into keyring.
- Wrong PIN / invalid token returns typed `privacy_recovery_token_invalid`.
- Detailed restore scenarios and proposed `.hrk` evolution are documented in:
  - `docs/PRIVACY_RECOVERY.md`

## Notes

- The old `age` + `holder-crypt` filter model is removed.
- Privacy checks and error codes now use `privacy_*` naming in API responses.
