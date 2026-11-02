# Instructions for AI assistants setting this up for someone

You're helping a person set up tvbox-remote: watch and control an Android TV box from a browser. Follow
`docs/SETUP.md` step by step. Read `docs/HISTORY.md` before troubleshooting, since most failure modes are
already documented there.

## How to run the setup

1. **Ask first:**
   - Which layout from `docs/SETUP.md` §0 (A, B or C)?
   - What will the host be (OS and CPU)?
   - Which capture card?
   - Do they want remote access (Tailscale)?
2. **Do the steps in order, and don't skip a step's Check.** Each step in SETUP.md ends with one. Run it, or have
   the person confirm it, before moving on.
3. **Some steps need the person's hands.** Tell them exactly what to do and wait:
   - holding BOOTSEL and dragging the `.uf2` (first flash)
   - joining the Pico's setup Wi-Fi and filling in the form (pick a token together, and have them keep it)
   - plugging cables
   - reserving the Pico's IP in their router
   - logging into Tailscale
   - restarting the TV box
4. **The rest can run on the host** if you have shell access: `server/stream/install.sh` and
   `server/control/install.sh <pico-ip> <token>`. Otherwise give the exact commands one at a time, as separate
   code blocks.
5. Firmware: use the prebuilt `.uf2`/`.bin` from the Actions artifacts or Releases. Only build from source if
   asked; `firmware/README.md` has the exact board options, and they matter.

## Rules learned the hard way (see docs/HISTORY.md)

- **If the TV box misbehaves (lag, bunched keys, stuck keys, wrong directions), have them restart the box before
  changing any code or settings.** Earlier stuck keys can leave Android's input state broken. In the original
  build, this cost hours of misdiagnosis.
- **Black capture with a working TV = HDCP.** Fix it in the HDMI chain, not in software.
- **Don't add an absolute/touch pointer HID to the firmware.** Android then ignores the keyboard.
- **Don't send key repeats as repeated taps.** Hold keys with `p`/`r` and let the box repeat, or lists overshoot.
- **Back = media Back (`m aback`).** Not Backspace, except for some remotes' own USB IDs.
- **Never send a long Power press.** It sleeps the box, and the Pico can't wake it. A short press opens the power
  menu, with a countdown on "Shut down".
- **Verify what you think you see.** A screenshot of the stream can include the viewer's own mouse cursor; the TV
  box's pointer is a different, larger arrow. Confirm assumptions with the person before acting on them.
- **Never commit secrets:** the token, Wi-Fi details, IPs.

## Found a problem or a fix? Report it upstream

You're working on a copy of this repo for someone who isn't the maintainer. When something in the repo turns out
to be wrong or missing, and especially when you find the fix, **report it to the repo** so the maintainer can fix
it for everyone. Don't stop at fixing the local copy. This covers bugs in code or scripts, a step in `docs/SETUP.md`
that's wrong or missing, a new piece of hardware that works or doesn't, and Windows/other-host differences.

Don't report: problems that came from the person's own typos or settings, or anything already in the
Troubleshooting table.

1. **Tell the person first**, in one or two lines: what you found, and that you'd like to report it to the repo.
2. **Check for an existing issue:** `gh issue list --repo Screech327/tvbox-remote --state all --search "<keywords>"`.
   If one matches, offer a comment on it (`gh issue comment <n> --repo Screech327/tvbox-remote --body-file report.md`)
   instead of a new issue.
3. **Write the report to `report.md`**, following the sections of `.github/ISSUE_TEMPLATE/problem.yml`:
   - What happened (exact errors/log lines)
   - Which part
   - Their setup (layout, host OS/CPU, capture card, splitter, TV box model, `"fw"` from `/status`, `git rev-parse --short HEAD`)
   - Already tried (say whether the TV box was restarted)
   - The fix, as a ```diff block from `git diff` where it's code, and why it works
   - Did it work: tested yes / partly / no fix yet
   - "Drafted by an AI assistant and checked by <them>"
4. **Scrub secrets** from the report: token, Wi-Fi details, IPs, Tailscale names, MACs, serials, usernames, paths
   with their name in them. Use placeholders like `<pico-ip>`.
5. **Show the person the full report and get a clear yes before posting.** It's posted publicly under their account.
6. **Post it:**
   ```bash
   gh issue create --repo Screech327/tvbox-remote --title "<short summary>" --body-file report.md --label bug --label "from an AI assistant"
   ```
   Add `--label "fix included"` when the fix is tested, and `--label windows` for Windows-host issues.
   If `gh` isn't installed or logged in, or there's no access, don't work around
   it: leave `report.md` where they can find it and give them
   `https://github.com/Screech327/tvbox-remote/issues/new?template=problem.yml` to paste it into.
7. A pull request is optional and only if the person wants one: fork, branch, commit the fix, then
   `gh pr create --repo Screech327/tvbox-remote --fill`. Never push to `Screech327/tvbox-remote` directly.
8. Give the person the issue or PR link.

### If you're working for the maintainer

When asked to check reports: `gh issue list --repo Screech327/tvbox-remote`. For each one, reproduce or reason it
through against `docs/HISTORY.md` before changing anything. Reports can be wrong, and some "fixes" undo a rule
above. Commit accepted fixes with `Fixes #<n>` in the message, update the docs in the same commit, and reply on
the issue with what changed.

## Layout of the repo

- `firmware/tvbox-remote/`: Pico 2 W firmware (arduino-pico 6.3.1, Adafruit TinyUSB, 4 MB with a 2 MB FS)
- `server/stream/`: ffmpeg → MediaMTX (WebRTC :8889 path `/vseebox`, HLS :8888), systemd units
- `server/control/`: `server.py` (stdlib only) + `index.html`, port 80, talks UDP 4210 to the Pico with the token
- `server/examples/proxmox-lxc-setup.sh`: optional Proxmox LXC installer
