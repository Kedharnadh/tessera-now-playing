#include "now_playing.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome::now_playing {

static const char *const TAG = "now_playing";

using tessera::Font;
namespace ui = tessera::ui;

// Home Assistant's MediaPlayerEntityFeature: the bits that decide which keys a player has.
constexpr uint32_t F_PAUSE = 1, F_SEEK = 2, F_VOLUME_SET = 4, F_VOLUME_MUTE = 8, F_PREVIOUS = 16, F_NEXT = 32,
                   F_VOLUME_STEP = 1024, F_PLAY = 16384;
// Icons of Tessera's set (five hex digits each: six would read as a colour to tools/check.py).
constexpr uint32_t ICON_MUSIC = 0xF075A, ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_NEXT = 0xF04AD,
                   ICON_PREVIOUS = 0xF04AE, ICON_VOLUME_UP = 0xF075D, ICON_VOLUME_DOWN = 0xF075E,
                   ICON_VOLUME_HIGH = 0xF057E, ICON_VOLUME_MUTE = 0xF075F;

// A song's words: at most this many lines on a tile, in 48 bytes each (the app cuts a tile's texts there).
constexpr int MAX_LYRIC_LINES = 40;
constexpr int TILE_LYRIC_ROWS = 4;
constexpr int CARD_LYRIC_ROWS = 9;
// A wish shows at once (the core does not feed a plugin tile its own wish): the key keeps the new face until Home
// Assistant agrees, or this long has passed.
constexpr uint32_t FLIP_HOLD_MS = 6000;
// A seek stands on the bar until Home Assistant reports the player near it, or this long.
constexpr uint32_t SEEK_HOLD_MS = 6000;

static std::string plain(const tessera::Text &t) { return std::string(t.data(), t.size()); }

static void show_obj(lv_obj_t *o, bool on) {
  if (!o) return;
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// "3:07", or "1:02:07" from an hour on.
static std::string time_text(float seconds) {
  if (!(seconds > 0)) return "0:00";
  const long total = static_cast<long>(seconds);
  const long h = total / 3600, m = (total / 60) % 60, s = total % 60;
  char out[16];
  if (h > 0) std::snprintf(out, sizeof(out), "%ld:%02ld:%02ld", h, m, s);
  else std::snprintf(out, sizeof(out), "%ld:%02ld", m, s);
  return out;
}

// A line of the words, cut to the room a tile's text has without splitting a UTF-8 character.
static void set_line(char *out, size_t size, const char *text) {
  size_t n = strlen(text);
  if (n >= size) {
    n = size - 1;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
  }
  memcpy(out, text, n);
  out[n] = 0;
}

struct LyricLine {
  char text[52] = {};
  float at = 0;
};

// The lyrics the helper sensor gave, synced (a time a line) or plain (paced by the song's length).
struct Lyrics {
  bool synced = false;
  int count = 0;
  LyricLine line[MAX_LYRIC_LINES];

  void clear() {
    synced = false;
    count = 0;
  }

  void read(JsonObjectConst e) {
    clear();
    const char *st = e["state"] | "";
    if (!st[0] || !strcmp(st, "unavailable") || !strcmp(st, "unknown") || !strcmp(st, "none")) return;
    JsonArrayConst lines = e["attributes"]["lines"].as<JsonArrayConst>();
    if (lines.isNull()) return;
    JsonArrayConst times = e["attributes"]["times"].as<JsonArrayConst>();
    // Synced only when every line has its time: without them the words are paced by the song's length.
    const bool timed = !strcmp(st, "synced") && !times.isNull() && times.size() >= lines.size();
    synced = timed;
    int i = 0;
    for (JsonVariantConst v : lines) {
      if (i >= MAX_LYRIC_LINES) break;
      const char *t = v | "";
      if (!t[0]) continue;
      set_line(line[i].text, sizeof(line[i].text), t);
      line[i].at = timed && i < static_cast<int>(times.size()) ? (times[i] | 0.0f) : 0.0f;
      ++i;
    }
    count = i;
    if (count == 0) synced = false;
  }

  int index_at(float position, float duration) const {
    if (count == 0) return -1;
    if (!synced) {
      if (duration <= 0) return 0;
      const int i = static_cast<int>(position / duration * count);
      return std::max(0, std::min(count - 1, i));
    }
    int found = 0;
    for (int i = 0; i < count; ++i)
      if (line[i].at <= position + 0.05f) found = i;
    return found;
  }
};

// What a player says now, kept as its parts (docs/FIRMWARE_API.md, "A tile of an entity").
struct Media {
  tessera::Text state, title, artist, album;
  float position = 0, duration = 0;
  uint32_t position_at = 0;
  float volume = -1;
  bool muted = false;
  uint32_t features = 0;

  void read(JsonObjectConst data) {
    if (data["state"].isNull()) return;
    state = tessera::Text(data["state"] | "");
    JsonObjectConst a = data["attributes"].as<JsonObjectConst>();
    title = tessera::Text(a["media_title"] | "");
    artist = tessera::Text(a["media_artist"] | "");
    album = tessera::Text(a["media_album_name"] | "");
    position = a["media_position"] | 0.0f;
    duration = a["media_duration"] | 0.0f;
    position_at = a["media_position_updated_at"] | 0u;
    volume = a["volume_level"].isNull() ? -1.0f : (a["volume_level"] | 0.0f);
    muted = a["is_volume_muted"] | false;
    features = a["supported_features"] | 0u;
  }

  bool playing() const { return state == "playing" || state == "buffering"; }
  bool has_track() const { return title.size() > 0 || duration > 0; }

  // Where the song is now, from the moment the player last said.
  float at(uint32_t now) const {
    if (!playing() || !(duration > 0)) return position;
    if (position_at == 0 || now < position_at) return position;
    const float p = position + static_cast<float>(now - position_at);
    return std::min(p, duration);
  }
};

// A round key with its icon. Owned by a tile or a card and moved by its layout.
struct Key {
  enum Id : uint8_t { NONE, PREVIOUS, PLAY, NEXT, VOLUME_DOWN, VOLUME_UP, MUTE };
  Id id = NONE;
  lv_obj_t *body = nullptr, *icon = nullptr;
  int x = 0, y = 0, d = 0;
  bool shown = false;

  void make(lv_obj_t *parent) {
    body = ui::block(parent, theme::BUTTON);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_radius(body, LV_RADIUS_CIRCLE, 0);
    icon = ui::label(body, Font::ICON_SMALL, theme::INK);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
  }

  void show(bool on) {
    shown = on;
    show_obj(body, on);
  }

  void paint(Id new_id, uint32_t glyph, theme::Role fill, theme::Role ink,
             tessera::Tone tone = tessera::Tone::NORMAL) {
    id = new_id;
    ui::set_text(icon, ui::icon(glyph));
    ui::set_fill(body, tone, fill);
    ui::set_color(icon, ink);
  }

  void place(int px, int py, int size) {
    x = px;
    y = py;
    d = size;
    lv_obj_set_size(body, d, d);
    lv_obj_set_pos(body, x, y);
    const int ih = ui::line_height(Font::ICON_SMALL);
    lv_obj_set_width(icon, d);
    lv_obj_set_height(icon, ih);
    lv_obj_set_pos(icon, 0, (d - ih) / 2);
  }

  bool hit(const tessera::Point &p) const {
    return shown && p.x >= x && p.x < x + d && p.y >= y && p.y < y + d;
  }
};

// A bar that fills from the left, with the song's elapsed time at its left and its length at its right.
struct Progress {
  lv_obj_t *track = nullptr, *fill = nullptr, *elapsed = nullptr, *total = nullptr;
  int x = 0, y = 0, w = 0, h = 0;
  float last = -1.0f;

  void make(lv_obj_t *parent, bool times) {
    track = ui::block(parent, theme::TRACK);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(track, 0, 0);
    fill = ui::block(track, theme::ACCENT);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_pos(fill, 0, 0);
    if (times) {
      elapsed = ui::label(parent, Font::BODY, theme::MUTED);
      total = ui::label(parent, Font::BODY, theme::MUTED);
      lv_obj_set_style_text_align(total, LV_TEXT_ALIGN_RIGHT, 0);
    }
  }

  void show(bool on) {
    show_obj(track, on);
    show_obj(elapsed, on);
    show_obj(total, on);
  }

  void place(int px, int py, int width, int height, float fraction) {
    x = px;
    y = py;
    w = width;
    h = height;
    lv_obj_set_size(track, w, h);
    lv_obj_set_pos(track, x, y);
    lv_obj_set_style_radius(track, h / 2, 0);
    lv_obj_set_style_radius(fill, h / 2, 0);
    last = -1.0f;
    set(fraction);
  }

  void set(float fraction) {
    const float f = std::max(0.0f, std::min(1.0f, fraction));
    if (f == last) return;
    last = f;
    const int fw = std::max(0, std::min(w, static_cast<int>(w * f + 0.5f)));
    lv_obj_set_size(fill, fw, h);
  }

  void theme() {
    ui::set_fill(track, tessera::Tone::NORMAL, theme::TRACK);
    ui::set_fill(fill, tessera::Tone::NORMAL, theme::ACCENT);
    if (elapsed) ui::set_color(elapsed, theme::MUTED);
    if (total) ui::set_color(total, theme::MUTED);
  }
};

// The tile: the cover of what a media player plays, its title and artist, a bar, up to six keys, and (with the helper
// sensor set) the words of the song. One cell is the cover alone; one row the cover beside its words; one column the
// cover over them; a larger tile puts the words, the bar, the keys and the lyrics around it.
class PlayerTile : public tessera::Tile {
 public:
  explicit PlayerTile(NowPlaying *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    columns_ = c.columns;
    rows_ = c.rows;
    tile_ = c.tile;
    entity_ = tessera::Text(c.entity ? c.entity : "");
    lyrics_on_ = !std::string(c.options["lyrics"] | "").empty();
    pictures_ = ui::pictures();
    pad_ = ui::px(8);
    gap_ = ui::px(6);

    if (columns_ == 1 && rows_ == 1) mode_ = Mode::COVER;
    else if (rows_ == 1) mode_ = Mode::ROW;
    else if (columns_ == 1) mode_ = Mode::STACK;
    else mode_ = Mode::RICH;
    beside_ = width_ >= height_;
    title_face_ = mode_ == Mode::COVER ? Font::BODY_LARGE
                  : (mode_ == Mode::RICH && beside_ && width_ > ui::mm(70)) ? Font::HEADLINE
                                                                             : Font::TITLE;
    artist_face_ = (mode_ == Mode::RICH && beside_) ? Font::BODY_LARGE : Font::BODY;

    // The art: a soft block with a record mark, and over it the picture's place. A picture that is not there lets the
    // block and the mark show (Placeholder::NONE).
    art_block_ = ui::block(c.parent, theme::TRACK);
    lv_obj_remove_flag(art_block_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(art_block_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(art_block_, 0, 0);
    art_icon_ = ui::label(art_block_, Font::ICON, theme::MUTED);
    ui::set_text(art_icon_, ui::icon(ICON_MUSIC));
    lv_obj_set_style_text_align(art_icon_, LV_TEXT_ALIGN_CENTER, 0);
    if (pictures_) {
      cover_ = ui::picture(c.parent, tessera::Placeholder::NONE);
      if (!entity_.empty()) ui::set_picture(cover_, "cover:" + plain(entity_), tessera::Fit::COVER);
    }

    // The words.
    title_ = ui::label(c.parent, title_face_, theme::INK);
    artist_ = ui::label(c.parent, artist_face_, theme::INK_SOFT);
    note_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);

    progress_.make(c.parent, mode_ == Mode::RICH);

    for (auto &k : keys_) k.make(c.parent);

    if (lyrics_on_) {
      const int n = mode_ == Mode::RICH ? 3 : (mode_ == Mode::STACK && rows_ >= 4) ? 4 : 0;
      for (int i = 0; i < n; ++i) lyric_[i] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lyric_created_ = n;
    }

    layout();
    show();
  }

  void on_state(JsonObjectConst data) override {
    media_.read(data);
    read_lyrics(data);
    flip_until_ = 0;
    mute_flip_until_ = 0;
    show();
  }

  void on_tick(uint32_t epoch) override {
    (void) epoch;
    const float pos = media_.at(tessera::epoch());
    set_progress(pos);
    set_lyrics(pos);
    if (flip_until_ && esphome::millis() >= flip_until_) {
      flip_until_ = 0;
      place_keys();
    }
    if (mute_flip_until_ && esphome::millis() >= mute_flip_until_) {
      mute_flip_until_ = 0;
      place_keys();
    }
  }

  void on_theme() override {
    ui::set_fill(art_block_, tessera::Tone::NORMAL, theme::TRACK);
    ui::set_color(art_icon_, theme::MUTED);
    ui::set_color(title_, theme::INK);
    ui::set_color(artist_, theme::INK_SOFT);
    ui::set_color(note_, theme::MUTED);
    progress_.theme();
    place_keys();
    paint_lyrics();
  }

  void on_picture(lv_obj_t *place, tessera::PictureState state) override {
    if (place != cover_) return;
    picture_shown_ = state == tessera::PictureState::SHOWN;
    show();
  }

  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    for (int i = 0; i < key_count_; ++i)
      if (keys_[i].hit(p)) return press(keys_[i].id);
    if (!entity_.empty())
      tessera::open_card(plugin_->plugin_id(), "player", plain(entity_), tile_);
  }

 private:
  enum class Mode : uint8_t { COVER, ROW, STACK, RICH };

  void read_lyrics(JsonObjectConst data) {
    if (!lyrics_on_) return;
    JsonVariantConst e = data["entities"]["lyrics"];
    if (!e.isNull()) {
      lyrics_.read(e.as<JsonObjectConst>());
      lyrics_seen_ = true;
    } else if (!data["entities"].isNull()) {
      lyrics_.clear();
      lyrics_seen_ = true;
    }
  }

  // ---- the geometry, laid out once ----

  void layout() {
    switch (mode_) {
      case Mode::COVER: layout_cover(); break;
      case Mode::ROW: layout_row(); break;
      case Mode::STACK: layout_stack(); break;
      case Mode::RICH: layout_rich(); break;
    }
    if (title_) {
      ui::set_font(title_, title_face_);
      ui::set_font(artist_, artist_face_);
    }
    place_art();
  }

  void place_art() {
    lv_obj_set_size(art_block_, art_w_, art_h_);
    lv_obj_set_pos(art_block_, art_x_, art_y_);
    const int ih = ui::line_height(Font::ICON);
    lv_obj_set_size(art_icon_, art_w_, ih);
    lv_obj_set_pos(art_icon_, 0, (art_h_ - ih) / 2);
    if (cover_) {
      lv_obj_set_size(cover_, art_w_, art_h_);
      lv_obj_set_pos(cover_, art_x_, art_y_);
    }
  }

  void layout_cover() {
    art_x_ = 0;
    art_y_ = 0;
    art_w_ = width_;
    art_h_ = height_;
    // The title over the bottom of the cover, when there is no cover to show.
    title_x_ = pad_;
    title_y_ = height_ - pad_ - ui::line_height(title_face_);
    title_w_ = width_ - 2 * pad_;
    lv_obj_set_width(title_, title_w_);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
    note_x_ = pad_;
    note_y_ = (height_ - 2 * ui::line_height(Font::BODY)) / 2;
    note_w_ = width_ - 2 * pad_;
    keys_on_ = false;
    bar_ok_ = false;
    lyric_created_ = 0;
  }

  void layout_row() {
    int d = std::min(height_ - 2 * pad_, width_ * 45 / 100);
    d = std::max(d, ui::mm(8));
    art_x_ = pad_;
    art_y_ = (height_ - d) / 2;
    art_w_ = art_h_ = d;

    words_x_ = art_x_ + d + gap_;
    words_w_ = width_ - words_x_ - pad_;
    const int th = ui::line_height(title_face_), ah = ui::line_height(artist_face_);
    int y = pad_;
    title_x_ = words_x_;
    title_y_ = y;
    title_w_ = words_w_;
    lv_obj_set_width(title_, title_w_);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_LEFT, 0);
    y += th + ui::px(2);
    artist_x_ = words_x_;
    artist_y_ = y;
    artist_w_ = words_w_;
    lv_obj_set_width(artist_, artist_w_);
    y += ah;

    const int bar_h = std::max(ui::px(3), ui::px(4));
    int key_size = std::min(ui::mm(9), (words_w_ - 2 * gap_) / 3);
    int key_top = height_ - pad_ - key_size;
    keys_on_ = key_size >= ui::mm(7) && key_top - gap_ >= y + ui::px(4);
    if (keys_on_) {
      keys_x_ = words_x_;
      keys_w_ = words_w_;
      keys_bottom_ = height_ - pad_;
      keys_max_ = key_size;
      keys_volume_ = false;
    }
    int bar_y = y + ui::px(3);
    bar_y = std::min(bar_y, (keys_on_ ? key_top - gap_ : height_ - pad_) - bar_h);
    bar_ok_ = bar_y >= y;
    bar_x_ = words_x_;
    bar_y_ = bar_y;
    bar_w_ = words_w_;
    bar_h_ = bar_h;

    note_x_ = words_x_;
    note_y_ = y;
    note_w_ = words_w_;
    lyric_created_ = 0;
  }

  void layout_stack() {
    int d = std::min(width_ - 2 * pad_, height_ * 50 / 100);
    art_x_ = (width_ - d) / 2;
    art_y_ = pad_;
    art_w_ = art_h_ = d;

    int y = art_y_ + d + gap_;
    title_x_ = pad_;
    title_y_ = y;
    title_w_ = width_ - 2 * pad_;
    lv_obj_set_width(title_, title_w_);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_LEFT, 0);
    y += ui::line_height(title_face_) + ui::px(2);
    artist_x_ = pad_;
    artist_y_ = y;
    artist_w_ = title_w_;
    lv_obj_set_width(artist_, artist_w_);
    y += ui::line_height(artist_face_);

    const int bar_h = std::max(ui::px(3), ui::px(4));
    int key_size = std::min(ui::mm(10), (title_w_ - 2 * gap_) / 3);
    keys_on_ = rows_ >= 3 && key_size >= ui::mm(7);
    if (keys_on_) {
      keys_x_ = pad_;
      keys_w_ = title_w_;
      keys_bottom_ = height_ - pad_;
      keys_max_ = key_size;
      keys_volume_ = false;
    }
    int bar_y = y + ui::px(3);
    bar_y = std::min(bar_y, (keys_on_ ? keys_bottom_ - key_size - gap_ : height_ - pad_) - bar_h);
    bar_ok_ = bar_y >= y;
    bar_x_ = pad_;
    bar_y_ = bar_y;
    bar_w_ = title_w_;
    bar_h_ = bar_h;

    lyric_x_ = pad_;
    lyric_w_ = title_w_;
    lyric_y_ = bar_ok_ ? bar_y + bar_h + ui::px(4) : y + ui::px(2);
    const int bottom = keys_on_ ? keys_bottom_ - key_size - gap_ : height_ - pad_;
    lyric_h_ = std::max(0, bottom - lyric_y_);

    note_x_ = pad_;
    note_y_ = y;
    note_w_ = title_w_;
  }

  void layout_rich() {
    const int bar_h = std::max(ui::px(3), ui::px(4));
    if (beside_) {
      int d = std::min(height_ - 2 * pad_, width_ * 42 / 100);
      d = std::max(d, ui::mm(10));
      art_x_ = pad_;
      art_y_ = (height_ - d) / 2;
      art_w_ = art_h_ = d;
      words_x_ = art_x_ + d + gap_ * 2;
      words_w_ = width_ - words_x_ - pad_;
    } else {
      int d = std::min(width_ - 2 * pad_, height_ * 38 / 100);
      art_x_ = (width_ - d) / 2;
      art_y_ = pad_;
      art_w_ = art_h_ = d;
      words_x_ = pad_;
      words_w_ = width_ - 2 * pad_;
    }
    int y = beside_ ? pad_ : art_y_ + art_w_ + gap_;
    title_x_ = words_x_;
    title_y_ = y;
    title_w_ = words_w_;
    lv_obj_set_width(title_, title_w_);
    lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_LEFT, 0);
    y += ui::line_height(title_face_) + ui::px(2);
    artist_x_ = words_x_;
    artist_y_ = y;
    artist_w_ = words_w_;
    lv_obj_set_width(artist_, artist_w_);
    y += ui::line_height(artist_face_);

    keys_x_ = words_x_;
    keys_w_ = words_w_;
    keys_bottom_ = height_ - pad_;
    keys_volume_ = rows_ >= 3;
    keys_max_ = keys_volume_ ? ui::mm(10) : ui::mm(11);
    const int key_size = std::min(keys_max_, (keys_w_ - 2 * gap_) / 3);
    const int key_rows = keys_volume_ ? 2 : 1;
    const int key_block = key_rows * key_size + (key_rows - 1) * gap_;
    int key_top = keys_bottom_ - key_block;
    keys_on_ = key_size >= ui::mm(7);

    int bar_y = y + ui::px(3);
    bar_y = std::min(bar_y, (keys_on_ ? key_top - gap_ : keys_bottom_) - bar_h);
    bar_ok_ = bar_y >= y;
    bar_x_ = words_x_;
    bar_y_ = bar_y;
    bar_w_ = words_w_;
    bar_h_ = bar_h;

    lyric_x_ = words_x_;
    lyric_w_ = words_w_;
    lyric_y_ = bar_ok_ ? bar_y + bar_h + ui::px(4) : y + ui::px(2);
    const int bottom = keys_on_ ? key_top - gap_ : keys_bottom_;
    lyric_h_ = std::max(0, bottom - lyric_y_);

    note_x_ = words_x_;
    note_y_ = y;
    note_w_ = words_w_;
  }

  // ---- what it shows ----

  void show() {
    const std::string st = plain(media_.state);
    const std::string title = plain(media_.title);
    const std::string artist = plain(media_.artist);
    const std::string album = plain(media_.album);
    std::string note;
    if (entity_.empty()) note = plugin_->text("choose");
    else if (!st.empty() && (st == "unavailable" || st == "unknown")) note = plugin_->text("unavailable");
    else if (st == "off") note = plugin_->text("off");
    else if (!media_.has_track()) note = plugin_->text("nothing");

    const bool words = note.empty();
    const bool cover_only = mode_ == Mode::COVER;

    // The words.
    std::string meta = artist;
    if (!album.empty() && album != artist) meta += (ui::covers(" \xC2\xB7 ", artist_face_) ? " \xC2\xB7 " : ", ") + album;
    ui::set_text(note_, note);
    ui::set_text(title_, words ? title : "");
    ui::set_text(artist_, words ? meta : "");
    show_obj(note_, !note.empty());
    show_obj(title_, words && !(cover_only && picture_shown_));
    show_obj(artist_, words && !cover_only);
    if (words && !cover_only) {
      lv_obj_set_pos(title_, title_x_, title_y_);
      lv_obj_set_pos(artist_, artist_x_, artist_y_);
    }
    if (note.empty() == false) {
      lv_obj_set_pos(note_, note_x_, note_y_);
      lv_obj_set_width(note_, note_w_);
    }

    // The keys, the bar and the lyrics only when the song is there.
    words_ok_ = words && !entity_.empty();
    position_keys();
    bar_shown_ = words && bar_ok_ && media_.duration > 0;
    if (!bar_shown_) progress_.show(false);
    const float pos = media_.at(tessera::epoch());
    set_progress(pos);
    set_lyrics(pos);
  }

  void set_progress(float pos) {
    if (!bar_shown_) return;
    const float frac = media_.duration > 0 ? pos / media_.duration : 0.0f;
    const std::string elapsed = time_text(pos), total = time_text(media_.duration);
    const int lh = ui::line_height(Font::BODY);
    int width = bar_w_;
    if (progress_.elapsed) {
      const int ew = std::max(ui::text_width(elapsed, Font::BODY), ui::text_width("0:00", Font::BODY));
      const int tw = std::max(ui::text_width(total, Font::BODY), ui::text_width("0:00", Font::BODY));
      const int room = bar_w_ - ew - tw - 2 * gap_;
      if (room >= ui::mm(8)) {
        width = room;
        ui::set_text(progress_.elapsed, elapsed);
        ui::set_text(progress_.total, total);
        lv_obj_set_width(progress_.elapsed, ew);
        lv_obj_set_width(progress_.total, tw);
        lv_obj_set_pos(progress_.elapsed, bar_x_, bar_y_ + (bar_h_ - lh) / 2);
        lv_obj_set_pos(progress_.total, bar_x_ + bar_w_ - tw, bar_y_ + (bar_h_ - lh) / 2);
        progress_.place(bar_x_ + ew + gap_, bar_y_, width, bar_h_, frac);
        progress_.show(true);
        return;
      }
    }
    progress_.place(bar_x_, bar_y_, bar_w_, bar_h_, frac);
    progress_.show(true);
  }

  void hide_lyrics() {
    for (int i = 0; i < lyric_created_; ++i) show_obj(lyric_[i], false);
  }

  void set_lyrics(float pos) { paint_lyrics_at(pos); }

  void paint_lyrics() { paint_lyrics_at(media_.at(tessera::epoch())); }

  void paint_lyrics_at(float pos) {
    if (!lyrics_on_ || lyric_created_ == 0) return;
    if (!words_ok_) {
      hide_lyrics();
      return;
    }
    const int lh = ui::line_height(Font::BODY);
    const int rows = std::min(lyric_created_, lyric_h_ / (lh > 0 ? lh : 1));
    const int idx = lyrics_.count ? lyrics_.index_at(pos, media_.duration) : -1;
    if (!lyrics_seen_ || rows <= 0 || idx < 0) {
      if (lyrics_seen_ && lyrics_.count == 0 && rows > 0 && mode_ != Mode::COVER) {
        ui::set_text(lyric_[0], plugin_->text("no_lyrics"));
        ui::set_color(lyric_[0], theme::SUBTLE);
        lv_obj_set_width(lyric_[0], lyric_w_);
        lv_obj_set_height(lyric_[0], lh);
        lv_obj_set_pos(lyric_[0], lyric_x_, lyric_y_);
        show_obj(lyric_[0], true);
        for (int i = 1; i < lyric_created_; ++i) show_obj(lyric_[i], false);
        return;
      }
      hide_lyrics();
      return;
    }
    int start = idx - rows / 2;
    if (start + rows > lyrics_.count) start = lyrics_.count - rows;
    if (start < 0) start = 0;
    for (int i = 0; i < lyric_created_; ++i) {
      const int li = start + i;
      const bool on = i < rows && li >= 0 && li < lyrics_.count;
      if (!on) {
        show_obj(lyric_[i], false);
        continue;
      }
      ui::set_text(lyric_[i], lyrics_.line[li].text);
      ui::set_color(lyric_[i], li == idx ? theme::INK : theme::MUTED);
      lv_obj_set_width(lyric_[i], lyric_w_);
      lv_obj_set_height(lyric_[i], lh);
      lv_obj_set_pos(lyric_[i], lyric_x_, lyric_y_ + i * lh);
      show_obj(lyric_[i], true);
    }
  }

  // ---- the keys ----

  bool offered(int i) const {
    const bool known = media_.features != 0;
    switch (i) {
      case 0: return !known || (media_.features & F_PREVIOUS) != 0;
      case 1: return !known || (media_.features & (F_PAUSE | F_PLAY)) != 0;
      case 2: return !known || (media_.features & F_NEXT) != 0;
      case 3: return keys_volume_ && (media_.features & (F_VOLUME_STEP | F_VOLUME_SET)) != 0;
      case 4: return keys_volume_ && (media_.features & (F_VOLUME_STEP | F_VOLUME_SET)) != 0;
      case 5: return keys_volume_ && (media_.features & F_VOLUME_MUTE) != 0;
      default: return false;
    }
  }

  Key::Id key_id(int i) const {
    switch (i) {
      case 0: return Key::PREVIOUS;
      case 1: return Key::PLAY;
      case 2: return Key::NEXT;
      case 3: return Key::VOLUME_DOWN;
      case 4: return Key::VOLUME_UP;
      default: return Key::MUTE;
    }
  }

  void position_keys() {
    if (!keys_on_ || !words_ok_) {
      hide_keys();
      return;
    }
    int total = 0;
    for (int i = 0; i < 6; ++i) total += offered(i) ? 1 : 0;
    if (total == 0) {
      hide_keys();
      return;
    }
    const int per_row = 3;
    const int size = std::min(keys_max_, (keys_w_ - 2 * gap_) / per_row);
    if (size < ui::mm(6)) {
      hide_keys();
      return;
    }
    const int rows = (total + per_row - 1) / per_row;
    const int block = rows * size + (rows - 1) * gap_;
    const int top = keys_bottom_ - block;
    int slot = 0;
    for (int i = 0; i < 6; ++i) {
      if (!offered(i)) continue;
      const int row = slot / per_row;
      const int in_row = std::min(per_row, total - row * per_row);
      const int row_w = in_row * size + (in_row - 1) * gap_;
      const int x = keys_x_ + (keys_w_ - row_w) / 2 + (slot % per_row) * (size + gap_);
      const int y = top + row * (size + gap_);
      Key &k = keys_[slot];
      const Key::Id id = key_id(i);
      uint32_t glyph = 0;
      theme::Role fill = theme::BUTTON, ink = theme::INK;
      switch (id) {
        case Key::PREVIOUS: glyph = ICON_PREVIOUS; break;
        case Key::NEXT: glyph = ICON_NEXT; break;
        case Key::PLAY:
          glyph = shown_playing() ? ICON_PAUSE : ICON_PLAY;
          fill = theme::ACCENT;
          ink = theme::ON_ACCENT;
          break;
        case Key::VOLUME_DOWN: glyph = ICON_VOLUME_DOWN; break;
        case Key::VOLUME_UP: glyph = ICON_VOLUME_UP; break;
        case Key::MUTE: glyph = shown_muted() ? ICON_VOLUME_MUTE : ICON_VOLUME_HIGH; break;
        default: break;
      }
      k.paint(id, glyph, fill, ink);
      k.place(x, y, size);
      k.show(true);
      ++slot;
    }
    for (int i = slot; i < 6; ++i) keys_[i].show(false);
    key_count_ = slot;
  }

  void hide_keys() {
    for (auto &k : keys_) k.show(false);
    key_count_ = 0;
  }

  void place_keys() { position_keys(); }

  bool shown_playing() const {
    if (flip_until_ && esphome::millis() < flip_until_) return play_face_;
    return media_.playing();
  }

  bool shown_muted() const {
    if (mute_flip_until_ && esphome::millis() < mute_flip_until_) return mute_face_;
    return media_.muted;
  }

  // ---- a key ----

  void press(Key::Id id) {
    const std::string e = plain(entity_);
    if (e.empty()) return;
    switch (id) {
      case Key::PREVIOUS:
        tessera::action("media_player.media_previous_track", e);
        break;
      case Key::NEXT:
        tessera::action("media_player.media_next_track", e);
        break;
      case Key::PLAY:
        play_face_ = !shown_playing();
        flip_until_ = esphome::millis() + FLIP_HOLD_MS;
        tessera::wish(e, tessera::Wish::PLAYING, play_face_ ? "1" : "0");
        position_keys();
        break;
      case Key::VOLUME_DOWN:
      case Key::VOLUME_UP:
        // On a muted player a step is no sound: unmute first, as Tessera's own player face does.
        if (shown_muted()) {
          mute_face_ = false;
          mute_flip_until_ = esphome::millis() + FLIP_HOLD_MS;
          tessera::wish(e, tessera::Wish::MUTED, "0");
        }
        tessera::action(id == Key::VOLUME_UP ? "media_player.volume_up" : "media_player.volume_down", e);
        position_keys();
        break;
      case Key::MUTE:
        mute_face_ = !shown_muted();
        mute_flip_until_ = esphome::millis() + FLIP_HOLD_MS;
        tessera::wish(e, tessera::Wish::MUTED, mute_face_ ? "1" : "0");
        position_keys();
        break;
      default:
        break;
    }
  }

  NowPlaying *plugin_;
  Mode mode_ = Mode::COVER;
  tessera::Text entity_;
  Media media_;
  Lyrics lyrics_;
  Progress progress_;
  Key keys_[6];

  int width_ = 0, height_ = 0, columns_ = 1, rows_ = 1, tile_ = -1;
  int pad_ = 0, gap_ = 0;
  int art_x_ = 0, art_y_ = 0, art_w_ = 0, art_h_ = 0;
  int title_x_ = 0, title_y_ = 0, title_w_ = 0;
  int artist_x_ = 0, artist_y_ = 0, artist_w_ = 0;
  int note_x_ = 0, note_y_ = 0, note_w_ = 0;
  int bar_x_ = 0, bar_y_ = 0, bar_w_ = 0, bar_h_ = 0;
  int keys_x_ = 0, keys_w_ = 0, keys_bottom_ = 0, keys_max_ = 0, key_count_ = 0;
  int lyric_x_ = 0, lyric_y_ = 0, lyric_w_ = 0, lyric_h_ = 0, lyric_created_ = 0;

  Font title_face_ = Font::TITLE, artist_face_ = Font::BODY;
  bool beside_ = false, pictures_ = false, lyrics_on_ = false, lyrics_seen_ = false;
  bool picture_shown_ = false, keys_on_ = false, keys_volume_ = false, bar_ok_ = false, bar_shown_ = false;
  bool words_ok_ = false;
  bool play_face_ = false, mute_face_ = false;
  uint32_t flip_until_ = 0, mute_flip_until_ = 0;

  lv_obj_t *art_block_ = nullptr, *art_icon_ = nullptr, *cover_ = nullptr;
  lv_obj_t *title_ = nullptr, *artist_ = nullptr, *note_ = nullptr;
  lv_obj_t *lyric_[TILE_LYRIC_ROWS] = {};
};

// ---- The card ----

// The player large, opened by a tap on the tile: the cover, the title and artist, the seek bar with its times, the
// transport and volume keys, a volume bar, and the lyrics with the line being sung lit.
class PlayerCard : public tessera::Card {
 public:
  explicit PlayerCard(NowPlaying *plugin) : plugin_(plugin) {}

  void open(const tessera::CardContext &c) override {
    width_ = c.width;
    height_ = c.height;
    entity_ = tessera::Text(c.entity ? c.entity : "");
    pad_ = std::max(ui::px(12), ui::mm(4));
    gap_ = ui::px(10);
    beside_ = c.width > c.height;
    pictures_ = ui::pictures();

    // The bottom: the volume bar, then the keys (play the biggest). Laid out here; show() hides those the player lacks.
    const int key_size = std::min(ui::mm(13), (width_ - 2 * pad_ - 5 * gap_) * 10 / 62);
    keys_on_ = key_size >= ui::mm(7);
    key_size_ = std::max(ui::mm(7), key_size);
    play_size_ = key_size_ * 6 / 5;
    keys_h_ = play_size_;
    keys_y_ = height_ - pad_ - keys_h_;

    vol_row_h_ = ui::line_height(Font::BODY);
    vol_row_y_ = keys_on_ ? keys_y_ - gap_ - vol_row_h_ : height_ - pad_ - vol_row_h_;

    content_bottom_ = vol_row_y_ - gap_;
    if (content_bottom_ < pad_ + ui::mm(10)) content_bottom_ = height_ - pad_;

    // The art and the words: beside it on a card lying down, under it on one standing up.
    int content_h = std::max(0, content_bottom_ - pad_);
    if (beside_) {
      int d = std::min(content_h, width_ * 40 / 100);
      d = std::max(d, std::min(ui::mm(12), content_h));
      art_w_ = art_h_ = d;
      art_x_ = pad_;
      art_y_ = pad_ + (content_h - d) / 2;
      words_x_ = art_x_ + d + gap_ * 2;
      words_w_ = width_ - words_x_ - pad_;
    } else {
      int d = std::min(width_ - 2 * pad_, content_h * 42 / 100);
      d = std::max(d, std::min(ui::mm(12), width_ - 2 * pad_));
      art_w_ = art_h_ = d;
      art_x_ = (width_ - d) / 2;
      art_y_ = pad_;
      words_x_ = pad_;
      words_w_ = width_ - 2 * pad_;
    }
    title_face_ = (beside_ && words_w_ < ui::mm(80)) ? Font::TITLE : Font::HEADLINE;
    title_h_ = 2 * ui::line_height(title_face_);

    int y = beside_ ? pad_ : art_y_ + art_w_ + gap_;
    title_x_ = words_x_;
    title_y_ = y;
    y += title_h_ + ui::px(2);
    artist_x_ = words_x_;
    artist_y_ = y;
    artist_w_ = words_w_;
    y += ui::line_height(Font::BODY_LARGE);
    bar_h_ = std::max(ui::px(4), ui::px(5));
    bar_y_ = y + ui::px(6);
    bar_x_ = words_x_;
    bar_w_ = words_w_;
    lyric_x_ = words_x_;
    lyric_w_ = words_w_;
    lyric_y_ = bar_y_ + bar_h_ + ui::px(8);
    lyric_h_ = std::max(0, content_bottom_ - lyric_y_);

    // The art: a soft block with the record mark, and over it the cover's place.
    art_block_ = ui::block(c.parent, theme::TRACK);
    lv_obj_remove_flag(art_block_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(art_block_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(art_block_, 0, 0);
    art_icon_ = ui::label(art_block_, Font::ICON, theme::MUTED);
    ui::set_text(art_icon_, ui::icon(ICON_MUSIC));
    lv_obj_set_style_text_align(art_icon_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(art_block_, art_w_, art_h_);
    lv_obj_set_pos(art_block_, art_x_, art_y_);
    const int ih = ui::line_height(Font::ICON);
    lv_obj_set_size(art_icon_, art_w_, ih);
    lv_obj_set_pos(art_icon_, 0, (art_h_ - ih) / 2);
    if (pictures_) {
      cover_ = ui::picture(c.parent, tessera::Placeholder::NONE);
      lv_obj_set_size(cover_, art_w_, art_h_);
      lv_obj_set_pos(cover_, art_x_, art_y_);
      if (!entity_.empty()) ui::set_picture(cover_, "cover:" + plain(entity_), tessera::Fit::COVER);
    }

    // The words.
    title_ = ui::text(c.parent, title_face_, theme::INK);
    lv_obj_set_size(title_, words_w_, title_h_);
    lv_obj_set_pos(title_, title_x_, title_y_);
    artist_ = ui::label(c.parent, Font::BODY_LARGE, theme::INK_SOFT);
    lv_obj_set_width(artist_, artist_w_);
    lv_obj_set_pos(artist_, artist_x_, artist_y_);
    note_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(note_, words_w_, 2 * ui::line_height(Font::BODY));
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note_, words_x_, beside_ ? pad_ : art_y_ + art_h_ + gap_);

    progress_.make(c.parent, true);
    volume_.make(c.parent, false);
    volume_icon_ = ui::label(c.parent, Font::ICON_SMALL, theme::MUTED);
    volume_text_ = ui::label(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(volume_text_, LV_TEXT_ALIGN_RIGHT, 0);
    {
      const int icon_h = ui::line_height(Font::ICON_SMALL);
      const int icon_w = icon_h;
      const int text_w = ui::text_width("100%", Font::BODY) + ui::px(2);
      lv_obj_set_size(volume_icon_, icon_w, icon_h);
      lv_obj_set_pos(volume_icon_, pad_, vol_row_y_ + (vol_row_h_ - icon_h) / 2);
      lv_obj_set_size(volume_text_, text_w, ui::line_height(Font::BODY));
      lv_obj_set_pos(volume_text_, width_ - pad_ - text_w, vol_row_y_ + (vol_row_h_ - ui::line_height(Font::BODY)) / 2);
      vol_bar_x_ = pad_ + icon_w + gap_;
      vol_bar_w_ = std::max(0, width_ - pad_ - text_w - gap_ - vol_bar_x_);
      vol_bar_h_ = std::max(ui::px(4), ui::px(5));
      vol_bar_y_ = vol_row_y_ + (vol_row_h_ - vol_bar_h_) / 2;
    }

    for (auto &k : keys_) k.make(c.parent);

    lyric_font_ = Font::BODY_LARGE;
    for (int i = 0; i < CARD_LYRIC_ROWS; ++i) lyric_[i] = ui::label(c.parent, lyric_font_, theme::MUTED);
    lyric_created_ = CARD_LYRIC_ROWS;

    show();
  }

  void on_state(JsonObjectConst data) override {
    const std::string was = plain(media_.title);
    media_.read(data);
    read_lyrics(data);
    flip_until_ = 0;
    mute_flip_until_ = 0;
    if (seeking_ && (plain(media_.title) != was ||
                     std::fabs(media_.position - seek_target_) <= 3.0f))
      seeking_ = false;
    show();
  }

  void on_tick(uint32_t epoch) override {
    (void) epoch;
    if (seeking_ && esphome::millis() >= seek_until_) seeking_ = false;
    const float pos = shown_position();
    set_progress(pos);
    set_lyrics(pos);
    if (flip_until_ && esphome::millis() >= flip_until_) {
      flip_until_ = 0;
      position_keys();
    }
    if (mute_flip_until_ && esphome::millis() >= mute_flip_until_) {
      mute_flip_until_ = 0;
      position_keys();
      paint_volume();
    }
  }

  void on_theme() override {
    ui::set_fill(art_block_, tessera::Tone::NORMAL, theme::TRACK);
    ui::set_color(art_icon_, theme::MUTED);
    ui::set_color(title_, theme::INK);
    ui::set_color(artist_, theme::INK_SOFT);
    ui::set_color(note_, theme::MUTED);
    ui::set_color(volume_text_, theme::MUTED);
    ui::set_color(volume_icon_, theme::MUTED);
    progress_.theme();
    volume_.theme();
    position_keys();
    paint_volume();
    paint_lyrics();
  }

  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    for (int i = 0; i < key_count_; ++i)
      if (keys_[i].hit(p)) return press(keys_[i].id);

    const int slop = ui::mm(3);
    if (bar_shown_ && p.x >= seek_x_ && p.x <= seek_x_ + seek_w_ && p.y >= seek_y_ - slop &&
        p.y <= seek_y_ + seek_h_ + slop)
      return seek((p.x - seek_x_) / static_cast<float>(std::max(1, seek_w_)));

    if (volume_shown_ && p.x >= vol_bar_x_ && p.x <= vol_bar_x_ + vol_bar_w_ && p.y >= vol_row_y_ - slop &&
        p.y <= vol_row_y_ + vol_row_h_ + slop)
      return set_volume((p.x - vol_bar_x_) / static_cast<float>(std::max(1, vol_bar_w_)));
  }

 private:
  void read_lyrics(JsonObjectConst data) {
    JsonVariantConst e = data["entities"]["lyrics"];
    if (!e.isNull()) {
      lyrics_.read(e.as<JsonObjectConst>());
      lyrics_seen_ = true;
    } else if (!data["entities"].isNull()) {
      lyrics_.clear();
      lyrics_seen_ = true;
    }
  }

  void show() {
    const std::string st = plain(media_.state);
    const std::string title = plain(media_.title);
    const std::string artist = plain(media_.artist);
    const std::string album = plain(media_.album);
    std::string note;
    if (entity_.empty()) note = plugin_->text("choose");
    else if (!st.empty() && (st == "unavailable" || st == "unknown")) note = plugin_->text("unavailable");
    else if (st == "off") note = plugin_->text("off");
    else if (!media_.has_track()) note = plugin_->text("nothing");

    words_ok_ = note.empty();
    ui::set_text(note_, note);
    show_obj(note_, !note.empty());
    std::string meta = artist;
    if (!album.empty() && album != artist)
      meta += (ui::covers(" \xC2\xB7 ", Font::BODY_LARGE) ? " \xC2\xB7 " : ", ") + album;
    ui::set_text(title_, words_ok_ ? title : "");
    ui::set_text(artist_, words_ok_ ? meta : "");
    show_obj(title_, words_ok_ && !title.empty());
    show_obj(artist_, words_ok_ && !meta.empty());

    position_keys();
    bar_shown_ = words_ok_ && media_.duration > 0 && bar_y_ + bar_h_ <= content_bottom_;
    if (!bar_shown_) progress_.show(false);
    const float pos = shown_position();
    set_progress(pos);
    paint_volume();
    set_lyrics(pos);
  }

  float shown_position() const {
    if (seeking_) return seek_target_;
    return media_.at(tessera::epoch());
  }

  void set_progress(float pos) {
    if (!bar_shown_) return;
    const float frac = media_.duration > 0 ? pos / media_.duration : 0.0f;
    const std::string elapsed = time_text(pos), total = time_text(media_.duration);
    const int lh = ui::line_height(Font::BODY);
    const int ew = std::max(ui::text_width(elapsed, Font::BODY), ui::text_width("0:00", Font::BODY));
    const int tw = std::max(ui::text_width(total, Font::BODY), ui::text_width("0:00", Font::BODY));
    const int room = bar_w_ - ew - tw - 2 * gap_;
    ui::set_text(progress_.elapsed, elapsed);
    ui::set_text(progress_.total, total);
    lv_obj_set_width(progress_.elapsed, ew);
    lv_obj_set_width(progress_.total, tw);
    lv_obj_set_pos(progress_.elapsed, bar_x_, bar_y_ + (bar_h_ - lh) / 2);
    lv_obj_set_pos(progress_.total, bar_x_ + bar_w_ - tw, bar_y_ + (bar_h_ - lh) / 2);
    if (room >= ui::mm(10)) {
      seek_x_ = bar_x_ + ew + gap_;
      seek_w_ = room;
      progress_.place(seek_x_, bar_y_, seek_w_, bar_h_, frac);
    } else {
      seek_x_ = bar_x_;
      seek_w_ = bar_w_;
      progress_.place(seek_x_, bar_y_, seek_w_, bar_h_, frac);
    }
    seek_y_ = bar_y_;
    seek_h_ = bar_h_;
    progress_.show(true);
  }

  void paint_volume() {
    const bool on = words_ok_ && media_.volume >= 0;
    volume_shown_ = on;
    volume_.show(on);
    show_obj(volume_icon_, on);
    show_obj(volume_text_, on);
    if (!on) return;
    const float v = std::max(0.0f, std::min(1.0f, media_.volume));
    volume_.place(vol_bar_x_, vol_bar_y_, vol_bar_w_, vol_bar_h_, v);
    ui::set_text(volume_icon_, ui::icon(shown_muted() ? ICON_VOLUME_MUTE : ICON_VOLUME_HIGH));
    ui::set_tone(volume_icon_, shown_muted() ? tessera::Tone::BUSY : tessera::Tone::NORMAL, theme::MUTED);
    char percent[8];
    std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(v * 100.0f + 0.5f));
    ui::set_text(volume_text_, percent);
    ui::set_color(volume_text_, theme::MUTED);
  }

  void set_lyrics(float pos) { paint_lyrics_at(pos); }

  void paint_lyrics() { paint_lyrics_at(shown_position()); }

  void paint_lyrics_at(float pos) {
    if (lyric_created_ == 0) return;
    const int lh = ui::line_height(lyric_font_);
    const int rows = std::min(lyric_created_, lh > 0 ? lyric_h_ / lh : 0);
    if (!words_ok_ || rows <= 0) {
      hide_lyrics();
      return;
    }
    const int idx = lyrics_.count ? lyrics_.index_at(pos, media_.duration) : -1;
    if (!lyrics_seen_ || idx < 0) {
      if (lyrics_seen_ && lyrics_.count == 0) {
        ui::set_text(lyric_[0], plugin_->text("no_lyrics"));
        ui::set_color(lyric_[0], theme::SUBTLE);
        lv_obj_set_width(lyric_[0], lyric_w_);
        lv_obj_set_height(lyric_[0], lh);
        lv_obj_set_pos(lyric_[0], lyric_x_, lyric_y_);
        show_obj(lyric_[0], true);
        for (int i = 1; i < lyric_created_; ++i) show_obj(lyric_[i], false);
        return;
      }
      hide_lyrics();
      return;
    }
    int start = idx - rows / 2;
    if (start + rows > lyrics_.count) start = lyrics_.count - rows;
    if (start < 0) start = 0;
    for (int i = 0; i < lyric_created_; ++i) {
      const int li = start + i;
      const bool on = i < rows && li >= 0 && li < lyrics_.count;
      if (!on) {
        show_obj(lyric_[i], false);
        continue;
      }
      ui::set_text(lyric_[i], lyrics_.line[li].text);
      ui::set_color(lyric_[i], li == idx ? theme::INK : theme::MUTED);
      lv_obj_set_width(lyric_[i], lyric_w_);
      lv_obj_set_height(lyric_[i], lh);
      lv_obj_set_pos(lyric_[i], lyric_x_, lyric_y_ + i * lh);
      show_obj(lyric_[i], true);
    }
  }

  void hide_lyrics() {
    for (int i = 0; i < lyric_created_; ++i) show_obj(lyric_[i], false);
  }

  // ---- the keys ----

  bool offered(int i) const {
    const bool known = media_.features != 0;
    switch (i) {
      case 0: return !known || (media_.features & F_PREVIOUS) != 0;
      case 1: return true;
      case 2: return !known || (media_.features & F_NEXT) != 0;
      case 3: return (media_.features & (F_VOLUME_STEP | F_VOLUME_SET)) != 0;
      case 4: return (media_.features & (F_VOLUME_STEP | F_VOLUME_SET)) != 0;
      case 5: return (media_.features & F_VOLUME_MUTE) != 0;
      default: return false;
    }
  }

  Key::Id key_id(int i) const {
    switch (i) {
      case 0: return Key::PREVIOUS;
      case 1: return Key::PLAY;
      case 2: return Key::NEXT;
      case 3: return Key::VOLUME_DOWN;
      case 4: return Key::VOLUME_UP;
      default: return Key::MUTE;
    }
  }

  uint32_t glyph_of(const Key::Id id) const {
    switch (id) {
      case Key::PREVIOUS: return ICON_PREVIOUS;
      case Key::NEXT: return ICON_NEXT;
      case Key::PLAY: return shown_playing() ? ICON_PAUSE : ICON_PLAY;
      case Key::VOLUME_DOWN: return ICON_VOLUME_DOWN;
      case Key::VOLUME_UP: return ICON_VOLUME_UP;
      case Key::MUTE: return shown_muted() ? ICON_VOLUME_MUTE : ICON_VOLUME_HIGH;
      default: return 0;
    }
  }

  void position_keys() {
    if (!keys_on_ || !words_ok_) {
      for (auto &k : keys_) k.show(false);
      key_count_ = 0;
      return;
    }
    int total = 0, total_w = 0;
    for (int i = 0; i < 6; ++i) {
      if (!offered(i)) continue;
      ++total;
      total_w += key_id(i) == Key::PLAY ? play_size_ : key_size_;
    }
    if (total == 0) {
      for (auto &k : keys_) k.show(false);
      key_count_ = 0;
      return;
    }
    total_w += (total - 1) * gap_;
    int x = (width_ - total_w) / 2;
    int slot = 0;
    for (int i = 0; i < 6; ++i) {
      if (!offered(i)) continue;
      const Key::Id id = key_id(i);
      const bool play = id == Key::PLAY;
      const int size = play ? play_size_ : key_size_;
      Key &k = keys_[slot];
      k.paint(id, glyph_of(id), play ? theme::ACCENT : theme::BUTTON, play ? theme::ON_ACCENT : theme::INK);
      k.place(x, keys_y_ + (keys_h_ - size) / 2, size);
      k.show(true);
      x += size + gap_;
      ++slot;
    }
    for (int i = slot; i < 6; ++i) keys_[i].show(false);
    key_count_ = slot;
  }

  bool shown_playing() const {
    if (flip_until_ && esphome::millis() < flip_until_) return play_face_;
    return media_.playing();
  }

  bool shown_muted() const {
    if (mute_flip_until_ && esphome::millis() < mute_flip_until_) return mute_face_;
    return media_.muted;
  }

  void press(Key::Id id) {
    const std::string e = plain(entity_);
    if (e.empty()) return;
    switch (id) {
      case Key::PREVIOUS:
        tessera::action("media_player.media_previous_track", e);
        break;
      case Key::NEXT:
        tessera::action("media_player.media_next_track", e);
        break;
      case Key::PLAY:
        play_face_ = !shown_playing();
        flip_until_ = esphome::millis() + FLIP_HOLD_MS;
        tessera::wish(e, tessera::Wish::PLAYING, play_face_ ? "1" : "0");
        position_keys();
        break;
      case Key::VOLUME_DOWN:
      case Key::VOLUME_UP:
        if (shown_muted()) {
          mute_face_ = false;
          mute_flip_until_ = esphome::millis() + FLIP_HOLD_MS;
          tessera::wish(e, tessera::Wish::MUTED, "0");
        }
        tessera::action(id == Key::VOLUME_UP ? "media_player.volume_up" : "media_player.volume_down", e);
        position_keys();
        break;
      case Key::MUTE:
        mute_face_ = !shown_muted();
        mute_flip_until_ = esphome::millis() + FLIP_HOLD_MS;
        tessera::wish(e, tessera::Wish::MUTED, mute_face_ ? "1" : "0");
        position_keys();
        paint_volume();
        break;
      default:
        break;
    }
  }

  void seek(float fraction) {
    const std::string e = plain(entity_);
    if (e.empty() || !(media_.duration > 0) || (media_.features & F_SEEK) == 0) return;
    const float f = std::max(0.0f, std::min(1.0f, fraction));
    seek_target_ = f * media_.duration;
    seeking_ = true;
    seek_until_ = esphome::millis() + SEEK_HOLD_MS;
    JsonDocument d;
    d["seek_position"] = seek_target_;
    tessera::action("media_player.media_seek", e, d);
    set_progress(seek_target_);
    set_lyrics(seek_target_);
  }

  void set_volume(float fraction) {
    const std::string e = plain(entity_);
    if (e.empty() || (media_.features & F_VOLUME_SET) == 0) return;
    const float f = std::max(0.0f, std::min(1.0f, fraction));
    media_.volume = f;
    JsonDocument d;
    d["volume_level"] = f;
    tessera::action("media_player.volume_set", e, d);
    paint_volume();
  }

  NowPlaying *plugin_;
  tessera::Text entity_;
  Media media_;
  Lyrics lyrics_;
  Progress progress_, volume_;
  Key keys_[6];

  int width_ = 0, height_ = 0, pad_ = 0, gap_ = 0;
  int art_x_ = 0, art_y_ = 0, art_w_ = 0, art_h_ = 0;
  int title_x_ = 0, title_y_ = 0, title_h_ = 0, artist_x_ = 0, artist_y_ = 0, artist_w_ = 0;
  int bar_x_ = 0, bar_y_ = 0, bar_w_ = 0, bar_h_ = 0, content_bottom_ = 0;
  int seek_x_ = 0, seek_y_ = 0, seek_w_ = 0, seek_h_ = 0;
  int keys_y_ = 0, keys_h_ = 0, key_size_ = 0, play_size_ = 0, key_count_ = 0;
  int vol_row_y_ = 0, vol_row_h_ = 0, vol_bar_x_ = 0, vol_bar_y_ = 0, vol_bar_w_ = 0, vol_bar_h_ = 0;
  int lyric_x_ = 0, lyric_y_ = 0, lyric_w_ = 0, lyric_h_ = 0, lyric_created_ = 0;

  Font title_face_ = Font::HEADLINE, lyric_font_ = Font::BODY_LARGE;
  bool beside_ = false, pictures_ = false, keys_on_ = false, words_ok_ = false;
  bool bar_shown_ = false, volume_shown_ = false, lyrics_seen_ = false;
  bool seeking_ = false, play_face_ = false, mute_face_ = false;
  float seek_target_ = 0;
  uint32_t seek_until_ = 0, flip_until_ = 0, mute_flip_until_ = 0;

  lv_obj_t *art_block_ = nullptr, *art_icon_ = nullptr, *cover_ = nullptr;
  lv_obj_t *title_ = nullptr, *artist_ = nullptr, *note_ = nullptr;
  lv_obj_t *volume_icon_ = nullptr, *volume_text_ = nullptr;
  lv_obj_t *lyric_[CARD_LYRIC_ROWS] = {};
};

// ---- The component ----

void NowPlaying::setup() {
  add_tile("player", [this]() { return new PlayerTile(this); });
  add_card("player", [this]() { return new PlayerCard(this); }, true);
}

void NowPlaying::dump_config() { ESP_LOGCONFIG(TAG, "Now playing"); }

}  // namespace esphome::now_playing
