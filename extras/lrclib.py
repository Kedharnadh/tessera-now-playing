#!/usr/bin/env python3
"""Lyrics for the Tessera now_playing plugin, from LRCLIB.

The Home Assistant `command_line` sensor in README.md runs this with the player's title, artist, album and length. It
prints one JSON object on stdout, which the sensor's `value_template` and `json_attributes` pick up:

    {"state": "synced", "lines": ["...", "..."], "times": [1.2, 4.5]}
    {"state": "plain",  "lines": ["...", "..."], "times": []}
    {"state": "none",   "lines": [],               "times": []}

`synced` has a time for every line; `plain` has the words without times and the screen paces them over the song's
length; `none` means LRCLIB had nothing (an instrumental, or a song it does not know). The lookup follows
Kedharnadh/ha-lyrics-card: LRCLIB's exact `/api/get`, then `/api/search`, YouTube noise stripped from title and artist,
a best match ranked by title, artist, album and length, synced words first, and timings transferred from a synced
candidate to plain words when their lines agree.

Standard library only (urllib, json, hashlib). The plugin's README has the sensor YAML. A copy of the script lives at
`extras/lrclib.py` in the plugin folder.
"""
import hashlib
import json
import os
import re
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

API = "https://lrclib.net/api"
UA = "Tessera-now_playing/0.1.0 (https://github.com/Kedharnadh)"
TTL_OK = 180 * 86400     # words found: half a year
TTL_MISS = 86400         # nothing found: a day
MAX_LINES = 40           # the screen's own buffer (now_playing.cpp, MAX_LYRIC_LINES)
MAX_BYTES = 48           # the app cuts a tile's text at 48 bytes
READ_PACE = 4.5          # seconds a plain line takes when timings cannot be transferred
TIMEOUT = 9

NOISE = re.compile(
    r"\((?:[^)]*(?:remaster|version|edit|mix|mono|stereo|live|bonus|deluxe|explicit|clean|radio|album|single|"
    r"expanded|anniversary|reissue|take\s*\d|part\s*\d)[^)]*)\)|\[[^\]]*\]|\{[^}]*\}",
    re.I,
)
FEAT = re.compile(r"\s*[-\u2013\u2014(]?\s*(?:feat|ft|featuring|with)\b.*$", re.I)
LRC_RE = re.compile(r"\[(\d{1,3}):([0-5]?\d)(?:[.:](\d{1,3}))?\]")
DIACRITICS = re.compile(r"[\u0300-\u036f]")


# ---- matching, as ha-lyrics-card does it ----------------------------------------------------------------

def squash(value):
    text = str(value or "").lower()
    text = unicodedata.normalize("NFD", text)
    text = DIACRITICS.sub("", text)
    text = NOISE.sub(" ", text)
    text = FEAT.sub("", text)
    text = re.sub(r"['\u2019`]", "", text)
    text = text.replace("&", " and ")
    text = re.sub(r"[^a-z0-9]+", " ", text)
    return text.strip()


def video_title(title):
    text = str(title or "")
    text = re.sub(
        r"\s*(?:\[|\()\s*(?:official\s+(?:lyric(?:s)?\s?video|video|audio)|lyric(?:s)?\s?video|audio|visualizer|hd|"
        r"hq|4k|8k|1080p|720p|480p|explicit)[^)\]]*(?:\)|\])\s*$",
        "",
        text,
        flags=re.I,
    )
    text = re.sub(
        r"\s*[-\u2013\u2014|]\s*(?:official\s+(?:lyric(?:s| video)?|video|audio)|lyric(?:s| video)?|audio|visualizer)\s*$",
        "",
        text,
        flags=re.I,
    )
    return text.strip()


def video_artist(artist):
    text = str(artist or "").strip()
    text = re.sub(r"\s*[-\u2013\u2014]\s*(?:topic|vevo)\s*$", "", text, flags=re.I).strip()
    if re.match(r"^(youtube|yt)$", text, re.I):
        return ""
    return text


def rank(record, want):
    track = squash(record.get("trackName") or record.get("name") or "")
    artist = squash(record.get("artistName") or "")
    album = squash(record.get("albumName") or "")
    wt, wa, wal = squash(want["title"]), squash(want["artist"]), squash(want["album"])
    score = 0
    if track and track == wt:
        score += 100
    elif track and wt and (wt in track or track in wt):
        score += 45
    if artist and wa:
        if artist == wa:
            score += 50
        elif artist in wa or wa in artist:
            score += 20
    if album and wal and album == wal:
        score += 25
    if want["duration"] > 0 and record.get("duration"):
        score += max(0, 30 - abs(record["duration"] - want["duration"]) * 2)
    return score


# ---- LRC, plain words and timing transfer ---------------------------------------------------------------

def parse_lrc(source):
    lines = []
    if not source:
        return lines
    for raw in str(source).splitlines():
        times = []
        text = ""
        cursor = 0
        for match in LRC_RE.finditer(raw):
            times.append(int(match.group(1)) * 60 + int(match.group(2)) + (float("0." + match.group(3)) if match.group(3) else 0.0))
            text += raw[cursor:match.start()]
            cursor = match.end()
        if not times:
            continue
        text = re.sub(r"\s+", " ", text + raw[cursor:]).strip()
        if not text:
            continue
        for stamp in times:
            lines.append((stamp, text))
    lines.sort(key=lambda item: item[0])
    return lines


def split_plain(source):
    out = []
    for line in str(source or "").splitlines():
        line = line.strip()
        if line and not re.match(r"^\[[\d:.]+\]$", line):
            out.append(line)
    return out


def fill_gaps(stamps):
    known = [i for i, stamp in enumerate(stamps) if stamp is not None]
    if len(known) < 2:
        for i in range(len(stamps)):
            if stamps[i] is None:
                stamps[i] = i * READ_PACE
        return stamps
    total, count = 0.0, 0
    for i in range(1, len(known)):
        total += (stamps[known[i]] - stamps[known[i - 1]]) / (known[i] - known[i - 1])
        count += 1
    step = min(10.0, max(1.5, total / count))
    for i in range(known[0] + 1, len(stamps)):
        if stamps[i] is None:
            stamps[i] = stamps[i - 1] + step
    for i in range(known[0] - 1, -1, -1):
        if stamps[i] is None:
            stamps[i] = max(0.0, stamps[i + 1] - step)
    return stamps


def transfer_timings(plain, keys, candidates):
    donor, best_hits = None, 0
    for candidate in candidates:
        if not candidate.get("syncedLyrics"):
            continue
        synced = parse_lrc(candidate["syncedLyrics"])
        if len(synced) < 4:
            continue
        hits = sum(1 for _, text in synced if squash(text) in keys)
        if hits > best_hits:
            best_hits, donor = hits, synced
    if not donor or best_hits < 4:
        return None
    stamps = [None] * len(plain)
    cursor, found_hits = 0, 0
    for stamp, text in donor:
        key = squash(text)
        if not key:
            continue
        found = -1
        for i in range(cursor, min(len(plain), cursor + 6)):
            if squash(plain[i]) == key:
                found = i
                break
        if found < 0:
            continue
        stamps[found] = stamp
        cursor = found + 1
        found_hits += 1
    if found_hits < max(3, len(plain) * 0.3):
        return None
    return fill_gaps(stamps)


# ---- the lookup -----------------------------------------------------------------------------------------

def get_json(path, params):
    """The answer, and whether it was the service's answer at all (a network failure is no answer)."""
    clean = {key: value for key, value in params.items() if value not in (None, "")}
    url = API + path + ("?" + urllib.parse.urlencode(clean) if clean else "")
    for attempt in range(2):
        try:
            request = urllib.request.Request(url, headers={"Accept": "application/json", "User-Agent": UA})
            with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
                return json.loads(response.read().decode("utf-8", "replace")), True
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None, True
            if error.code == 429 and attempt == 0:
                time.sleep(1.5)
                continue
            return None, error.code < 500
        except (urllib.error.URLError, OSError, ValueError):
            if attempt == 0:
                time.sleep(0.4)
                continue
            return None, False
    return None, False


def fetch(want):
    """(answer, cacheable). The answer: {"state", "lines", "times"} or None on a network failure."""
    title = video_title(want["title"]) or str(want["title"] or "").strip()
    if not title:
        return {"state": "none", "lines": [], "times": []}, True
    artist = video_artist(want["artist"]) or str(want["artist"] or "").strip()
    want = {"title": title, "artist": artist, "album": str(want["album"] or "").strip(), "duration": want["duration"]}

    records = []
    answer, ok = get_json(
        "/get",
        {
            "track_name": title,
            "artist_name": artist,
            "album_name": want["album"],
            "duration": str(round(want["duration"])) if want["duration"] > 0 else "",
        },
    )
    if not ok:
        return None, False
    if isinstance(answer, dict) and answer.get("trackName") is not None:
        records = [answer]
    else:
        # A 404 (or a get without a record): search by the words and the artist, then wider.
        queries = [
            {"track_name": title, "artist_name": artist},
            {"q": (artist + " " + title).strip()},
        ]
        short = re.sub(r"\s*[-\u2013\u2014].+$", "", title).strip()
        if short and short != title:
            queries.append({"q": short})
        for query in queries:
            answer, ok = get_json("/search", query)
            if not ok:
                return None, False
            if isinstance(answer, list) and answer:
                records = answer
                break

    usable = [r for r in records if r and not r.get("instrumental") and (r.get("syncedLyrics") or r.get("plainLyrics"))]
    if not usable:
        if any(r and r.get("instrumental") for r in records):
            return {"state": "none", "lines": [], "times": []}, True
        return {"state": "none", "lines": [], "times": []}, True

    usable.sort(key=lambda record: rank(record, want), reverse=True)
    best = usable[0]
    if best.get("syncedLyrics"):
        synced = parse_lrc(best["syncedLyrics"])
        if synced:
            return make_synced(synced), True
    plain = split_plain(best.get("plainLyrics"))
    if not plain:
        return {"state": "none", "lines": [], "times": []}, True
    keys = {squash(line) for line in plain if squash(line)}
    stamps = transfer_timings(plain, keys, usable)
    if stamps:
        return make_synced(list(zip(stamps, plain))), True
    return {"state": "plain", "lines": cut_lines(plain), "times": []}, True


def cut(text):
    """At most MAX_BYTES of UTF-8, never inside a character."""
    raw = str(text).encode("utf-8")
    if len(raw) <= MAX_BYTES:
        return str(text)
    raw = raw[:MAX_BYTES]
    while raw and (raw[-1] & 0xC0) == 0x80:
        raw = raw[:-1]
    return raw.decode("utf-8", "ignore")


def cut_lines(lines):
    out = []
    for line in lines[:MAX_LINES]:
        line = cut(line)
        if line:
            out.append(line)
    return out


def make_synced(lines):
    """[(seconds, text), ...] as the sensor's lines and times, trimmed together to MAX_LINES."""
    lines = lines[:MAX_LINES]
    texts = cut_lines([text for _, text in lines])
    times = [round(stamp, 2) for stamp, _ in lines][: len(texts)]
    return {"state": "synced", "lines": texts, "times": times}


# ---- cache ----------------------------------------------------------------------------------------------

def cache_dir():
    return os.environ.get("LRCLIB_CACHE_DIR") or os.path.join(os.path.dirname(os.path.abspath(__file__)), ".lrclib_cache")


def cache_read(key):
    try:
        with open(os.path.join(cache_dir(), key + ".json"), "r", encoding="utf-8") as handle:
            record = json.load(handle)
    except (OSError, ValueError):
        return None
    ttl = TTL_OK if record.get("data", {}).get("state") != "none" else TTL_MISS
    if time.time() - float(record.get("at", 0)) > ttl:
        return None
    return record.get("data")


def cache_write(key, data):
    try:
        os.makedirs(cache_dir(), exist_ok=True)
        path = os.path.join(cache_dir(), key + ".json")
        with open(path + ".tmp", "w", encoding="utf-8") as handle:
            json.dump({"at": time.time(), "data": data}, handle, ensure_ascii=False)
        os.replace(path + ".tmp", path)
    except OSError:
        pass


def main(argv):
    title = argv[1] if len(argv) > 1 else ""
    artist = argv[2] if len(argv) > 2 else ""
    album = argv[3] if len(argv) > 3 else ""
    try:
        duration = float(argv[4]) if len(argv) > 4 else 0.0
    except ValueError:
        duration = 0.0
    want = {"title": title.strip(), "artist": artist.strip(), "album": album.strip(), "duration": max(0.0, duration)}
    if not want["title"]:
        print(json.dumps({"state": "none", "lines": [], "times": []}, ensure_ascii=False, separators=(",", ":")))
        return 0

    key = hashlib.sha1(
        "|".join([squash(want["title"]), squash(want["artist"]), squash(want["album"]), str(round(want["duration"]))]).encode("utf-8")
    ).hexdigest()
    answer = cache_read(key)
    if answer is None:
        answer, cacheable = fetch(want)
        if answer is None:
            answer = {"state": "none", "lines": [], "times": []}
        elif cacheable:
            cache_write(key, answer)
    print(json.dumps(answer, ensure_ascii=False, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
