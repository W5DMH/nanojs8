# Screenshots — capture guide

The main [README.md](../../README.md) references nine images in this folder. Until you drop them in, the README still renders fine (GitHub shows broken-image placeholders) — so there's no rush. Replace them whenever you have a quiet moment with the device.

## Photo tips

- **Native resolution is 320×240.** Anything larger gets scaled down by GitHub anyway. After cropping, ~640×480 or 800×600 is plenty.
- **Phone camera flat-on works fine.** Tilt the phone to perpendicular to the screen; an angle distorts the text.
- **Diffuse light** — overhead room light, not a window directly behind you. Direct light bounces glare off the screen.
- **Crop to just the screen bezel.** No need to show the keyboard or surroundings.
- **PNG is preferred but JPG is fine.** If you save as JPG, rename the file (e.g. `01-home.jpg`) and update the path in `README.md` accordingly.

If you want pixel-perfect captures instead of phone photos, the L7.16+ firmware has a screen capture path planned but it's not wired up yet. Phone photos are completely acceptable for v0.7-alpha.

---

## What to photograph for each filename

| Filename | Screen | How to get to it | What to capture |
|---|---|---|---|
| `01-home.png` | HOME | Default screen after boot, once configured | The full status panel with rows filled in. Ideally with at least UTC clock set, CAT connected (cyan freq), PTT idle, and a non-zero mailbox count for visual richness. |
| `02-setup.png` | SETUP | From HOME, trackball RIGHT (4 ticks); or boot fresh | All 7 rows visible: Call, Grid, Groups, Units, Freq, Radio, UTC. Show with values filled in, no row in edit mode (clean focus indicator on one row is fine). |
| `03-heard.png` | HEARD | From SETUP, trackball RIGHT | A few callsigns visible in the list. Ideally with a mix of SNR colors (green / yellow / gray) to show the color coding. If the band is dead, even one or two entries is enough. |
| `04-all.png` | ALL | From HEARD, trackball RIGHT | A few entries showing both heartbeat (`FROM` only) and directed (`FROM>TO`) patterns. Body wrapping visible on one entry if possible. |
| `05-directed.png` | DIRECTED | From ALL, trackball RIGHT | A few messages addressed to you or to @GHOSTNET. If empty, ask KD8PGB or another tester to send you one for the photo. |
| `06-inbox.png` | INBOX | From DIRECTED, trackball RIGHT | A few entries showing different lifecycle states if possible — at minimum one UNREAD (`>`) entry. Cursor visible on one row. |
| `07-inbox-detail.png` | INBOX_DETAIL | From INBOX, press Enter on a message | Full message detail screen: header (`MSG #N <state>`), kv block (From/To/At/SNR/Freq), body wrapping, footer hint. |
| `08-compose.png` | COMPOSE | From INBOX, trackball RIGHT | The form with TO, CMD, FOR (if visible), TEXT, SEND. Ideally with sample values typed in (e.g. TO: `KD8PGB`, CMD: `MSG`, TEXT: `hello from the field`). |
| `09-allcall.png` | ALLCALL | From COMPOSE, trackball RIGHT | The three-row menu (HEARTBEAT / QUERY MSGS / CQ) with one row in focus. |

---

## Updating the README after capture

Drop the .png files into this folder. Filenames match the table above (e.g. `01-home.png`).

To commit:
```bash
git add docs/screenshots/
git commit -m "Add screen photos to README"
git push
```

GitHub re-renders the README within a few seconds. No new release tag needed for documentation updates — the screenshots show up wherever `https://github.com/W5DMH/nanojs8` is viewed.

If you want them on the install site too (`https://w5dmh.github.io/nanojs8/`), the next tagged release will redeploy Pages and pick them up automatically.
