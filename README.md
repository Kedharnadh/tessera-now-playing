# Now playing

The album art, title and artist of any Home Assistant media player, on a tile that uses its size: one cell is the
cover alone, one row the cover beside its words, one column the cover over them, and a larger tile adds the progress
bar, the keys and, if you set it up, the words of the song. A tap opens the full card:
the cover large, the title over two lines, the artist and album, a seek bar that takes a tap, the transport and volume
keys, a volume bar, and the lyric window with the line being sung lit.

It works with every `media_player`, whether it plays from Music Assistant, Spotify, a TV, a radio or your own files.
The cover comes from the player's own `entity_picture` through Home Assistant's proxy, so the screen needs no network
permission and never reaches the internet. The keys are the ones the player says it has.

## Set up

1. Add the plugin to a screen in Tessera (**Add with a link**,
   `https://github.com/Kedharnadh/tessera-now-playing`), or pick **Now playing** from the Plugins page's Community
   list, and let the screen build once.
2. In **Layout**, place the **Now playing** tile from the library's Plugins group where you want it. Its size decides
   what it shows; drag the edge to make it larger. In the inspector, under **Player**, pick the `media_player` the tile
   shows and, if you like, the **Lyrics sensor** from the next steps.
3. The screen must be allowed to perform Home Assistant actions (the ESPHome integration's option, which Tessera turns
   on when it adds the screen). The plugin asks for the media player's play/pause, previous and next track, volume up
   and down, mute, seek and volume set.

### Lyrics (optional)

The lyrics come from [LRCLIB](https://lrclib.net), the same free service the
[ha-lyrics-card](https://github.com/Kedharnadh/ha-lyrics-card) uses, and are brought to the screen through a Home
Assistant sensor. The screen itself still needs no internet.

1. Copy [`extras/lrclib.py`](extras/lrclib.py) to `/config/now_playing_lrclib.py` on the Home Assistant machine (the
   **File editor** or **Studio Code Server** add-on can do it; a Samba share works too).
2. Put this in `configuration.yaml`, under the existing top-level keys, and change `media_player.living_room` to your
   player. Restart Home Assistant, or reload **Command line** and **Template** entities.

```yaml
command_line:
  - sensor:
      name: Now playing lyrics
      unique_id: now_playing_lyrics
      # The script prints one JSON object. The quotes in the arguments are shlex quotes, not a shell: the template
      # strips the quote characters a title or artist may carry so one word cannot break the command.
      command: >-
        python3 /config/now_playing_lrclib.py
        "{{ state_attr('media_player.living_room', 'media_title') | default('', true) | replace('"', ' ') | replace("'", ' ') }}"
        "{{ state_attr('media_player.living_room', 'media_artist') | default('', true) | replace('"', ' ') | replace("'", ' ') }}"
        "{{ state_attr('media_player.living_room', 'media_album_name') | default('', true) | replace('"', ' ') | replace("'", ' ') }}"
        "{{ state_attr('media_player.living_room', 'media_duration') | int(0) }}"
      value_template: "{{ value_json.state }}"
      json_attributes:
        - lines
        - times
      scan_interval: 15
      command_timeout: 20
```

3. In the tile's inspector, under **Lyrics sensor**, pick `sensor.now_playing_lyrics`. The next time the player's
   title changes, the lines appear on the tile and on the card.

The sensor's state is `synced` (every line has its time), `plain` (the words without times, which the screen paces over
the song's length) or `none` (no lyrics for this song, or an instrumental). The `lines` and `times` attributes carry the
words, cut to 48 bytes a line and at most 40 lines for the tile's small data budget. The script caches its answers for
half a year (a day for a song it did not find), so the sensor does not ask LRCLIB again for a song it knows.

The lookup mirrors ha-lyrics-card: LRCLIB's exact `/api/get`, then `/api/search`; "Official Video", "Lyrics" and the
like are stripped from the title and "- Topic" from the artist; the best match is ranked by title, artist, album and
length; synced words are used first, and when only plain words exist the script transfers the timings of a synced
version whose lines agree with them. Without a transferable version the screen paces the plain lines by itself.

## Good to know

- **Keys.** The tile shows previous, play/pause and next when there is room for them; on three rows by two columns or
  more it adds volume down, volume up and mute when the player reports them. Without a `supported_features` value the
  tile shows the three transport keys, so a player that reports nothing still has a play key. Play/pause and mute go
  through Tessera's own wish, which is why the key changes face at once; the player's answer then confirms it.
- **The bar.** The tile's progress bar needs a larger tile and its times room beside the bar; where that is missing the
  bar fills the width alone. The card's seek bar takes a tap anywhere along it and holds the new position until Home
  Assistant reports the player near it.
- **Volume.** A tap along the card's volume bar sets the level. Stepping the volume down or up while the player is
  muted takes the mute off first, as Tessera's own player does.
- **Cover.** One cell is the cover alone; when the player has no picture (a radio stream, a board without the memory
  for pictures such as the CYD or the Hosyond 4-inch) the tile shows a record mark and the title over the bottom of it.
  A tap on the tile anywhere but a key opens the card.
- **Lyrics on the tile.** The tile shows up to three lines on a tile of two rows by two columns or more, and up to four
  on a one-column tile four rows high or more, as the room allows. The line being sung is in the screen's ink, the
  others muted.
- **No lyrics.** Without the sensor the plugin draws the cover, the words, the bar and the keys and leaves the lyric
  space to them. With the sensor set but no words for the song, the card says "No lyrics for this song".

## Privacy

The screen asks Home Assistant for the player's cover and sends it the keys you tap; it never reaches the internet and
the manifest asks for no network permission. When you set up the lyric sensor, Home Assistant (not the screen) asks
LRCLIB with the song's title, artist, album and length. LRCLIB sees your Home Assistant's address and those four
values; nothing else of your home. The answers are cached on the Home Assistant machine under
`/config/.lrclib_cache/`.

## Licence

MIT. The lyrics lookup follows [Kedharnadh/ha-lyrics-card](https://github.com/Kedharnadh/ha-lyrics-card) (MIT).
