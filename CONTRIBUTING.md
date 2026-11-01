# Reporting problems and fixes

Found something that doesn't work, or figured out what fixes it? **Tell the repo, don't just fix your own copy.**
The maintainer checks the report, applies the fix properly, and everyone gets it.

You don't change any files in this repo yourself. There are two ways to report, depending on what you have:

| You have… | Do this |
|---|---|
| A problem, with or without a fix | **Open an issue** with the **Problem or fix** form: *Issues → New issue*. Paste the fix (a `git diff` or a description) into it. |
| A tested code fix and you know GitHub | Optional: **fork → branch → pull request**. The maintainer reviews and merges it. |

Either way, the maintainer makes the final change in the repo.

## Before posting

1. **Remove secrets:** your token, Wi-Fi name and password, IP addresses, Tailscale names, serial numbers.
   Write `<pico-ip>`, `<token>`, `<host-ip>` instead.
2. **Key or mouse problems: restart the TV box first.** A stuck key can leave Android's input state broken in a way
   that looks exactly like a firmware bug. See [docs/HISTORY.md](docs/HISTORY.md).
3. Search the existing issues. If yours is there, add a comment instead of a new issue.

## Using an AI assistant (Claude, etc.)

Your assistant can write and post the report for you. [CLAUDE.md](CLAUDE.md) tells it how: it drafts the report,
shows it to you, and posts it **only after you approve**, from your own GitHub account. If it can't post (no GitHub
CLI, or no access), it saves the report as a file and gives you a link to paste it into.

## Access

While the repo is private, you need to be added as a collaborator to open issues. Ask the maintainer. Once it's
public, anyone with a GitHub account can.
