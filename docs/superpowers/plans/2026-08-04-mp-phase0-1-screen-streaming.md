# Фазы Ф0 и Ф1: сервер со стримингом экрана и тонкий клиент — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** один игрок играет обычную партию Cataclysm-DDA, подключившись по TCP к отдельному
серверному процессу, который гоняет симуляцию и стримит ему экран.

**Architecture:** сервер — отдельная цель сборки, линкующая четвёртый бэкенд curses. Вместо
рисования он копит изменившиеся ячейки в композитный буфер и отправляет их клиенту; вместо
чтения клавиатуры читает нажатия из сокета. Клиент — тот же бинарь в режиме `--connect`,
без мира, контента и правил: применяет присланные ячейки и шлёт ввод.

**Tech Stack:** C++17, собственная curses-реализация проекта (`src/cursesport.cpp`), вендоренный
`imtui` для рендера imgui в текст, `JsonOut`/`JsonValue` для управляющих сообщений, свои
BSD-сокеты с шимом под Winsock, Catch2 (`tests/cata_test`).

## Global Constraints

- **Спек:** `docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md`. Разделы 5.1, 6, 11.
- **Версия протокола:** `MP_PROTOCOL_VERSION = 1`. Первым сообщением, несовпадение — отказ.
- **Кодировка:** гибрид. Управляющие сообщения — JSON через существующие `JsonOut`/`JsonValue`.
  Экран — компактный бинарь с прогонами.
- **Транспорт:** только свои сокеты. Никаких новых внешних зависимостей, ни SDL_net, ни asio.
- **`TCP_NODELAY` обязателен** на обоих концах. Без него Нейгл добавит десятки миллисекунд
  ровно на пути «нажатие → экран».
- **Симуляция не знает о сети.** Весь новый код — в `src/mp/`. Правки в существующих файлах
  допускаются только в: `src/cursesport.h` (условие `TUI`), `src/ncurses_def.cpp` (директива
  guard), `src/cata_imgui.cpp` (выбор бэкенда imgui), `src/main.cpp` (аргументы), `Makefile`.
  Любая другая правка существующего файла — повод остановиться и пересогласовать.
- **Никаких потоков в этих двух фазах.** Планировщик и поток ввода-вывода — это Ф3.
  Сервер здесь однопоточный: нарисовал, сбросил экран, заблокировался на приёме.
- **Сборка тестов:** `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1`.
  Тесты, требующие серверного бэкенда, собираются отдельно:
  `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`.
- **Запуск тестов:** `tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/` (каталог создать заранее).
- **Стиль:** отступы и форматирование — `make astyle`. Комментарии по-английски, как во всём
  `src/`. Русский — только в документах плана и спека.
- **Коммиты:** после каждой задачи, с префиксами `feat(mp):`, `test(mp):`, `build:`, `docs:`.

---

## Обзор файлов

**Создаются:**

| Файл | Ответственность |
|---|---|
| `src/mp/mp_frame.h`, `.cpp` | Кадрирование «длина плюс тело» поверх потока байт |
| `src/mp/mp_cell.h` | Ячейка экрана и сетка ячеек с построчным отслеживанием изменений |
| `src/mp/mp_screen_codec.h`, `.cpp` | Кодирование и декодирование изменившихся строк |
| `src/mp/mp_protocol.h`, `.cpp` | Типы сообщений, JSON-управление, сериализация `input_event` |
| `src/mp/mp_transport.h` | Абстрактный транспорт |
| `src/mp/mp_loopback_transport.h`, `.cpp` | Транспорт в памяти для тестов, с инжекцией задержки |
| `src/mp/mp_socket.h`, `.cpp` | Платформенный шим над сокетами |
| `src/mp/mp_tcp_transport.h`, `.cpp` | TCP-транспорт |
| `src/mp/mp_server_screen.h`, `.cpp` | Композитный буфер сервера и его связь с `cata_cursesport::WINDOW` |
| `src/mp/mp_curses_server.cpp` | Серверный бэкенд curses (только при `MP_SERVER`) |
| `src/mp/mp_imgui_server.cpp` | Выкладка `ImTui::TScreen` в буфер сервера (только при `MP_SERVER`) |
| `src/mp/mp_server_main.h`, `.cpp` | Точка входа сервера |
| `src/mp/mp_client_main.h`, `.cpp` | Точка входа клиента |
| `tests/mp_frame_test.cpp` | Кадрирование |
| `tests/mp_screen_codec_test.cpp` | Кодек экрана |
| `tests/mp_protocol_test.cpp` | Управляющие сообщения и `input_event` |
| `tests/mp_loopback_transport_test.cpp` | Транспорт в памяти |
| `tests/mp_tcp_transport_test.cpp` | TCP на 127.0.0.1 |
| `tests/mp_server_session_test.cpp` | Сессия целиком (только при `MP_SERVER`) |
| `doc/MULTIPLAYER.md` | Как поднять сервер и подключиться |

**Изменяются:**

| Файл | Что именно |
|---|---|
| `src/cursesport.h:5-7` | Условие определения `TUI` |
| `src/ncurses_def.cpp:1` | Guard, чтобы ncurses-бэкенд не компилировался в серверной цели |
| `src/cata_imgui.cpp:105,119,133,156-157,167,173` | Выбор бэкенда imgui для `MP_SERVER` |
| `src/main.cpp:301` (`first_pass_arguments`), `:636` (`main`) | Аргументы `--server`, `--connect` и развилка |
| `Makefile:1105` и цели сборки | Флаг `MP_SERVER`, цель `cataclysm-server`, imtui-исходники |

**Почему именно так.** Разделение на `mp_frame` / `mp_screen_codec` / `mp_protocol` /
`mp_transport` сделано ради тестируемости: первые три — чистые функции над байтами, они
проверяются в обычной сборке тестов, без сервера, без сокетов и без игры. Всё, что требует
серверного бэкенда, собрано в двух файлах, компилируемых только в серверной цели.

---

### Task 1: Кадрирование

**Files:**
- Create: `src/mp/mp_frame.h`, `src/mp/mp_frame.cpp`
- Test: `tests/mp_frame_test.cpp`

**Interfaces:**
- Consumes: ничего.
- Produces:
  - `std::vector<uint8_t> mp_frame_encode( const std::vector<uint8_t> &payload )`
  - `class mp_frame_reader` с методами `void feed( const uint8_t *data, size_t len )` и
    `std::optional<std::vector<uint8_t>> next()`
  - `constexpr size_t MP_MAX_FRAME_BYTES = 8u * 1024u * 1024u`

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_frame_test.cpp`:

```cpp
#include "cata_catch.h"

#include <cstdint>
#include <vector>

#include "mp/mp_frame.h"

static std::vector<uint8_t> bytes( const std::string &s )
{
    return std::vector<uint8_t>( s.begin(), s.end() );
}

TEST_CASE( "mp_frame round trips a single payload", "[mp]" )
{
    const std::vector<uint8_t> payload = bytes( "hello" );
    const std::vector<uint8_t> frame = mp_frame_encode( payload );

    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    const std::optional<std::vector<uint8_t>> got = reader.next();
    REQUIRE( got.has_value() );
    CHECK( *got == payload );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame reassembles a payload split across feeds", "[mp]" )
{
    // A byte-at-a-time feed is the worst case a socket can hand us: the reader
    // must yield nothing until the very last byte of the frame arrives.
    const std::vector<uint8_t> payload = bytes( "a longer payload than four bytes" );
    const std::vector<uint8_t> frame = mp_frame_encode( payload );

    mp_frame_reader reader;
    for( size_t i = 0; i + 1 < frame.size(); ++i ) {
        reader.feed( &frame[i], 1 );
        REQUIRE_FALSE( reader.next().has_value() );
    }
    reader.feed( &frame[frame.size() - 1], 1 );

    const std::optional<std::vector<uint8_t>> got = reader.next();
    REQUIRE( got.has_value() );
    CHECK( *got == payload );
}

TEST_CASE( "mp_frame yields several payloads from one feed", "[mp]" )
{
    std::vector<uint8_t> stream = mp_frame_encode( bytes( "one" ) );
    const std::vector<uint8_t> second = mp_frame_encode( bytes( "two" ) );
    stream.insert( stream.end(), second.begin(), second.end() );

    mp_frame_reader reader;
    reader.feed( stream.data(), stream.size() );

    REQUIRE( reader.next() == bytes( "one" ) );
    REQUIRE( reader.next() == bytes( "two" ) );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame rejects an oversized length header", "[mp]" )
{
    // A hostile or corrupt peer must not be able to make us allocate freely.
    std::vector<uint8_t> frame = { 0xFF, 0xFF, 0xFF, 0xFF };

    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    CHECK( reader.failed() );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame accepts an empty payload", "[mp]" )
{
    const std::vector<uint8_t> frame = mp_frame_encode( {} );
    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    const std::optional<std::vector<uint8_t>> got = reader.next();
    REQUIRE( got.has_value() );
    CHECK( got->empty() );
}
```

- [ ] **Step 2: Прогнать тест и убедиться, что он падает**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: ошибка компиляции `mp/mp_frame.h: No such file or directory`.

- [ ] **Step 3: Написать заголовок**

Создать `src/mp/mp_frame.h`:

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_FRAME_H
#define CATA_SRC_MP_MP_FRAME_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Message framing for the multiplayer transports.
//
// The wire carries "length plus body": a four byte big-endian payload length
// followed by exactly that many bytes. Both transports (TCP and the in-memory
// loopback used by tests) hand raw bytes to mp_frame_reader, which reassembles
// whole payloads regardless of how the stream was chopped up.

// Largest payload we will accept. A full redraw of a very large terminal is a
// few hundred kilobytes, so this is generous; its purpose is to keep a corrupt
// or hostile length header from provoking an unbounded allocation.
constexpr size_t MP_MAX_FRAME_BYTES = 8u * 1024u * 1024u;

std::vector<uint8_t> mp_frame_encode( const std::vector<uint8_t> &payload );

class mp_frame_reader
{
    public:
        // Appends raw bytes as they arrive from the peer.
        void feed( const uint8_t *data, size_t len );

        // Returns the next complete payload, or nullopt when the buffered
        // bytes do not yet contain one.
        std::optional<std::vector<uint8_t>> next();

        // True once a malformed length header was seen. The stream cannot be
        // resynchronised after that, so the caller must drop the connection.
        bool failed() const {
            return failed_;
        }

    private:
        std::vector<uint8_t> buffer_;
        bool failed_ = false;
};

#endif // CATA_SRC_MP_MP_FRAME_H
```

- [ ] **Step 4: Написать реализацию**

Создать `src/mp/mp_frame.cpp`:

```cpp
#include "mp/mp_frame.h"

#include <algorithm>

static constexpr size_t header_bytes = 4;

std::vector<uint8_t> mp_frame_encode( const std::vector<uint8_t> &payload )
{
    std::vector<uint8_t> out;
    out.reserve( header_bytes + payload.size() );
    const uint32_t len = static_cast<uint32_t>( payload.size() );
    out.push_back( static_cast<uint8_t>( ( len >> 24 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( ( len >> 16 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( ( len >> 8 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( len & 0xFF ) );
    out.insert( out.end(), payload.begin(), payload.end() );
    return out;
}

void mp_frame_reader::feed( const uint8_t *const data, const size_t len )
{
    if( failed_ ) {
        return;
    }
    buffer_.insert( buffer_.end(), data, data + len );
}

std::optional<std::vector<uint8_t>> mp_frame_reader::next()
{
    if( failed_ || buffer_.size() < header_bytes ) {
        return std::nullopt;
    }
    const uint32_t len = ( static_cast<uint32_t>( buffer_[0] ) << 24 ) |
                         ( static_cast<uint32_t>( buffer_[1] ) << 16 ) |
                         ( static_cast<uint32_t>( buffer_[2] ) << 8 ) |
                         static_cast<uint32_t>( buffer_[3] );
    if( len > MP_MAX_FRAME_BYTES ) {
        failed_ = true;
        buffer_.clear();
        return std::nullopt;
    }
    if( buffer_.size() < header_bytes + len ) {
        return std::nullopt;
    }
    std::vector<uint8_t> payload( buffer_.begin() + header_bytes,
                                  buffer_.begin() + header_bytes + len );
    buffer_.erase( buffer_.begin(), buffer_.begin() + header_bytes + len );
    return payload;
}
```

- [ ] **Step 5: Прогнать тест и убедиться, что он проходит**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
mkdir -p /tmp/cata-mp-test
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Ожидается: пять тест-кейсов проходят.

- [ ] **Step 6: Коммит**

```bash
git add src/mp/mp_frame.h src/mp/mp_frame.cpp tests/mp_frame_test.cpp
git commit -m "feat(mp): add length-prefixed message framing"
```

---

### Task 2: Сетка ячеек и кодек экрана

**Files:**
- Create: `src/mp/mp_cell.h`, `src/mp/mp_screen_codec.h`, `src/mp/mp_screen_codec.cpp`
- Test: `tests/mp_screen_codec_test.cpp`

**Interfaces:**
- Consumes: `MP_MAX_FRAME_BYTES` из Task 1 (для проверки размера).
- Produces:
  - `struct mp_cell { std::string ch; uint8_t fg; uint8_t bg; }` с `operator==`
  - `class mp_screen_grid` — `mp_screen_grid( int w, int h )`, `resize( int, int )`,
    `width()`, `height()`, `at( int x, int y )`, `set( int x, int y, const mp_cell & )`,
    `mark_all_dirty()`, `dirty_rows()`, `clear_dirty()`, `any_dirty()`
  - `std::vector<uint8_t> mp_screen_encode( const mp_screen_grid & )`
  - `bool mp_screen_apply( mp_screen_grid &target, const std::vector<uint8_t> &payload )`

**Замечание о цветах.** `fg` и `bg` — индексы в 256-цветном пространстве. Значения 0–15
приходят из `cata_cursesport::cursecell` (базовые восемь плюс сдвиг на 8 за жирность и
мерцание, см. `src/cursesport.cpp:489-506`), 237 — `dark_gray`, значения выше 15 приходят
от imgui через imtui, который работает в ansi256. Клиент раскрашивает по своей палитре.

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_screen_codec_test.cpp`:

```cpp
#include "cata_catch.h"

#include <cstdint>
#include <string>
#include <vector>

#include "mp/mp_cell.h"
#include "mp/mp_screen_codec.h"

static mp_cell cell( const std::string &ch, const uint8_t fg = 7, const uint8_t bg = 0 )
{
    mp_cell c;
    c.ch = ch;
    c.fg = fg;
    c.bg = bg;
    return c;
}

TEST_CASE( "mp_screen_grid starts fully dirty", "[mp]" )
{
    // A freshly created grid has never been sent, so every row must go out.
    mp_screen_grid grid( 4, 3 );
    CHECK( grid.dirty_rows() == std::vector<int> { 0, 1, 2 } );
}

TEST_CASE( "mp_screen_grid marks only changed rows dirty", "[mp]" )
{
    mp_screen_grid grid( 4, 3 );
    grid.clear_dirty();
    REQUIRE_FALSE( grid.any_dirty() );

    grid.set( 2, 1, cell( "x" ) );
    CHECK( grid.dirty_rows() == std::vector<int> { 1 } );
}

TEST_CASE( "mp_screen_grid ignores a write that changes nothing", "[mp]" )
{
    // Redrawing the same content is extremely common in this game; if it
    // dirtied rows we would resend the whole screen on every keystroke.
    mp_screen_grid grid( 4, 3 );
    grid.set( 2, 1, cell( "x" ) );
    grid.clear_dirty();

    grid.set( 2, 1, cell( "x" ) );
    CHECK_FALSE( grid.any_dirty() );
}

TEST_CASE( "mp_screen codec transfers dirty rows only", "[mp]" )
{
    mp_screen_grid source( 8, 4 );
    mp_screen_grid target( 8, 4 );

    // Full initial sync.
    const std::vector<uint8_t> first = mp_screen_encode( source );
    REQUIRE( mp_screen_apply( target, first ) );
    source.clear_dirty();

    source.set( 3, 2, cell( "@", 3, 1 ) );
    const std::vector<uint8_t> delta = mp_screen_encode( source );
    source.clear_dirty();

    REQUIRE( mp_screen_apply( target, delta ) );
    CHECK( target.at( 3, 2 ) == cell( "@", 3, 1 ) );
    CHECK( target.at( 0, 0 ) == mp_cell() );
}

TEST_CASE( "mp_screen codec round trips a full screen of varied content", "[mp]" )
{
    mp_screen_grid source( 20, 6 );
    for( int y = 0; y < source.height(); ++y ) {
        for( int x = 0; x < source.width(); ++x ) {
            const uint8_t fg = static_cast<uint8_t>( ( x * 7 + y ) % 256 );
            const uint8_t bg = static_cast<uint8_t>( ( x + y * 3 ) % 16 );
            source.set( x, y, cell( x % 5 == 0 ? "é" : "#", fg, bg ) );
        }
    }

    mp_screen_grid target( 20, 6 );
    REQUIRE( mp_screen_apply( target, mp_screen_encode( source ) ) );

    for( int y = 0; y < source.height(); ++y ) {
        for( int x = 0; x < source.width(); ++x ) {
            REQUIRE( target.at( x, y ) == source.at( x, y ) );
        }
    }
}

TEST_CASE( "mp_screen codec compresses runs of identical cells", "[mp]" )
{
    // A blank 80x24 screen is 1920 identical cells; run-length encoding must
    // turn that into a handful of bytes, not two kilobytes.
    mp_screen_grid blank( 80, 24 );
    const std::vector<uint8_t> payload = mp_screen_encode( blank );
    CHECK( payload.size() < 400 );
}

TEST_CASE( "mp_screen_apply rejects a payload for a different screen size", "[mp]" )
{
    mp_screen_grid source( 8, 4 );
    mp_screen_grid target( 10, 4 );
    CHECK_FALSE( mp_screen_apply( target, mp_screen_encode( source ) ) );
}

TEST_CASE( "mp_screen_apply rejects a truncated payload", "[mp]" )
{
    mp_screen_grid source( 8, 4 );
    std::vector<uint8_t> payload = mp_screen_encode( source );
    payload.resize( payload.size() / 2 );

    mp_screen_grid target( 8, 4 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
}
```

- [ ] **Step 2: Прогнать тест и убедиться, что он падает**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: `mp/mp_cell.h: No such file or directory`.

- [ ] **Step 3: Написать `mp_cell.h`**

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_CELL_H
#define CATA_SRC_MP_MP_CELL_H

#include <cstdint>
#include <string>
#include <vector>

// One character cell of a player's screen.
//
// `ch` is the UTF-8 glyph, mirroring cata_cursesport::cursecell: an empty
// string marks the second column of a double-width character.
//
// `fg` and `bg` are indices into the 256 colour space. Values 0-15 come from
// cursesport, which folds bold and blink into the base colour by adding 8
// (src/cursesport.cpp:489-506); 237 is dark_gray; values above 15 come from
// imgui by way of imtui, which works in ansi256. The client maps them with
// its own palette.
struct mp_cell {
    std::string ch = " ";
    uint8_t fg = 7;
    uint8_t bg = 0;

    bool operator==( const mp_cell &rhs ) const {
        return ch == rhs.ch && fg == rhs.fg && bg == rhs.bg;
    }
    bool operator!=( const mp_cell &rhs ) const {
        return !operator==( rhs );
    }
};

// A screen of cells with per-row change tracking. Row granularity mirrors
// cata_cursesport::curseline::touched, so the server can copy the game's own
// notion of "what changed" straight through.
class mp_screen_grid
{
    public:
        mp_screen_grid( int width, int height );

        // Discards all content and marks the whole screen dirty.
        void resize( int width, int height );

        int width() const {
            return width_;
        }
        int height() const {
            return height_;
        }

        const mp_cell &at( int x, int y ) const;
        // Out of range writes are ignored, matching curses semantics.
        void set( int x, int y, const mp_cell &c );

        void mark_all_dirty();
        void clear_dirty();
        bool any_dirty() const;
        std::vector<int> dirty_rows() const;

    private:
        int width_ = 0;
        int height_ = 0;
        std::vector<mp_cell> cells_;
        std::vector<bool> dirty_;
};

#endif // CATA_SRC_MP_MP_CELL_H
```

- [ ] **Step 4: Написать `mp_screen_codec.h`**

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_SCREEN_CODEC_H
#define CATA_SRC_MP_MP_SCREEN_CODEC_H

#include <cstdint>
#include <vector>

class mp_screen_grid;

// Wire format of one screen update. All integers are big-endian.
//
//   u16 width, u16 height        -- of the sender's screen, for validation
//   u16 row_count
//   per row:
//     u16 row_index
//     u16 run_count
//     per run:
//       u16 repeat
//       u8  fg, u8 bg
//       u8  glyph_len, glyph_len bytes of UTF-8 (0 means a continuation cell)
//
// Only rows the grid reports as dirty are included. The caller is responsible
// for calling clear_dirty() afterwards -- encoding does not mutate the grid,
// so a failed send can simply be retried.
std::vector<uint8_t> mp_screen_encode( const mp_screen_grid &grid );

// Applies an encoded update to `target`. Returns false and leaves the target
// untouched when the payload is malformed or describes a different screen
// size; the caller must then drop the connection or request a full redraw.
bool mp_screen_apply( mp_screen_grid &target, const std::vector<uint8_t> &payload );

#endif // CATA_SRC_MP_MP_SCREEN_CODEC_H
```

- [ ] **Step 5: Написать `mp_screen_codec.cpp`**

```cpp
#include "mp/mp_screen_codec.h"

#include <cstddef>
#include <string>

#include "mp/mp_cell.h"

namespace
{

void put_u16( std::vector<uint8_t> &out, const int value )
{
    out.push_back( static_cast<uint8_t>( ( value >> 8 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( value & 0xFF ) );
}

class reader
{
    public:
        reader( const std::vector<uint8_t> &data ) : data_( data ) {}

        bool u8( uint8_t &out ) {
            if( pos_ >= data_.size() ) {
                return false;
            }
            out = data_[pos_++];
            return true;
        }
        bool u16( int &out ) {
            uint8_t hi = 0;
            uint8_t lo = 0;
            if( !u8( hi ) || !u8( lo ) ) {
                return false;
            }
            out = ( static_cast<int>( hi ) << 8 ) | static_cast<int>( lo );
            return true;
        }
        bool str( const size_t len, std::string &out ) {
            if( pos_ + len > data_.size() ) {
                return false;
            }
            out.assign( data_.begin() + pos_, data_.begin() + pos_ + len );
            pos_ += len;
            return true;
        }

    private:
        const std::vector<uint8_t> &data_;
        size_t pos_ = 0;
};

} // namespace

std::vector<uint8_t> mp_screen_encode( const mp_screen_grid &grid )
{
    const std::vector<int> rows = grid.dirty_rows();

    std::vector<uint8_t> out;
    put_u16( out, grid.width() );
    put_u16( out, grid.height() );
    put_u16( out, static_cast<int>( rows.size() ) );

    for( const int y : rows ) {
        put_u16( out, y );

        // Runs are built into a scratch buffer first, because the run count
        // precedes them on the wire and is not known until the row is walked.
        std::vector<uint8_t> runs;
        int run_count = 0;
        int x = 0;
        while( x < grid.width() ) {
            const mp_cell &first = grid.at( x, 0 + y );
            int repeat = 1;
            while( x + repeat < grid.width() && grid.at( x + repeat, y ) == first ) {
                ++repeat;
            }
            put_u16( runs, repeat );
            runs.push_back( first.fg );
            runs.push_back( first.bg );
            runs.push_back( static_cast<uint8_t>( first.ch.size() ) );
            runs.insert( runs.end(), first.ch.begin(), first.ch.end() );
            ++run_count;
            x += repeat;
        }

        put_u16( out, run_count );
        out.insert( out.end(), runs.begin(), runs.end() );
    }

    return out;
}

bool mp_screen_apply( mp_screen_grid &target, const std::vector<uint8_t> &payload )
{
    reader r( payload );
    int width = 0;
    int height = 0;
    int row_count = 0;
    if( !r.u16( width ) || !r.u16( height ) || !r.u16( row_count ) ) {
        return false;
    }
    if( width != target.width() || height != target.height() ) {
        return false;
    }

    // Decode into a scratch list first so a truncated payload cannot leave the
    // target half-updated.
    struct pending {
        int x;
        int y;
        mp_cell cell;
    };
    std::vector<pending> writes;

    for( int i = 0; i < row_count; ++i ) {
        int y = 0;
        int run_count = 0;
        if( !r.u16( y ) || !r.u16( run_count ) || y < 0 || y >= height ) {
            return false;
        }
        int x = 0;
        for( int j = 0; j < run_count; ++j ) {
            int repeat = 0;
            uint8_t fg = 0;
            uint8_t bg = 0;
            uint8_t glyph_len = 0;
            if( !r.u16( repeat ) || !r.u8( fg ) || !r.u8( bg ) || !r.u8( glyph_len ) ) {
                return false;
            }
            mp_cell c;
            c.fg = fg;
            c.bg = bg;
            if( !r.str( glyph_len, c.ch ) ) {
                return false;
            }
            if( repeat <= 0 || x + repeat > width ) {
                return false;
            }
            for( int k = 0; k < repeat; ++k ) {
                writes.push_back( pending{ x + k, y, c } );
            }
            x += repeat;
        }
        if( x != width ) {
            return false;
        }
    }

    for( const pending &w : writes ) {
        target.set( w.x, w.y, w.cell );
    }
    return true;
}
```

- [ ] **Step 6: Написать `mp_cell` реализацию inline в заголовке или отдельным файлом**

Добавить `src/mp/mp_cell.cpp`:

```cpp
#include "mp/mp_cell.h"

#include <algorithm>

mp_screen_grid::mp_screen_grid( const int width, const int height )
{
    resize( width, height );
}

void mp_screen_grid::resize( const int width, const int height )
{
    width_ = std::max( 0, width );
    height_ = std::max( 0, height );
    cells_.assign( static_cast<size_t>( width_ ) * height_, mp_cell() );
    dirty_.assign( height_, true );
}

const mp_cell &mp_screen_grid::at( const int x, const int y ) const
{
    static const mp_cell out_of_range;
    if( x < 0 || y < 0 || x >= width_ || y >= height_ ) {
        return out_of_range;
    }
    return cells_[static_cast<size_t>( y ) * width_ + x];
}

void mp_screen_grid::set( const int x, const int y, const mp_cell &c )
{
    if( x < 0 || y < 0 || x >= width_ || y >= height_ ) {
        return;
    }
    mp_cell &dest = cells_[static_cast<size_t>( y ) * width_ + x];
    if( dest == c ) {
        return;
    }
    dest = c;
    dirty_[y] = true;
}

void mp_screen_grid::mark_all_dirty()
{
    dirty_.assign( height_, true );
}

void mp_screen_grid::clear_dirty()
{
    dirty_.assign( height_, false );
}

bool mp_screen_grid::any_dirty() const
{
    return std::any_of( dirty_.begin(), dirty_.end(), []( const bool d ) {
        return d;
    } );
}

std::vector<int> mp_screen_grid::dirty_rows() const
{
    std::vector<int> rows;
    for( int y = 0; y < height_; ++y ) {
        if( dirty_[y] ) {
            rows.push_back( y );
        }
    }
    return rows;
}
```

- [ ] **Step 7: Прогнать тесты и убедиться, что они проходят**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Ожидается: тринадцать тест-кейсов (пять из Task 1 плюс восемь новых) проходят.

- [ ] **Step 8: Коммит**

```bash
git add src/mp/mp_cell.h src/mp/mp_cell.cpp src/mp/mp_screen_codec.h \
        src/mp/mp_screen_codec.cpp tests/mp_screen_codec_test.cpp
git commit -m "feat(mp): add screen cell grid and delta codec"
```

---

### Task 3: Управляющие сообщения

**Files:**
- Create: `src/mp/mp_protocol.h`, `src/mp/mp_protocol.cpp`
- Test: `tests/mp_protocol_test.cpp`

**Interfaces:**
- Consumes: `mp_screen_encode` из Task 2 (только для константы вида сообщения).
- Produces:
  - `constexpr int MP_PROTOCOL_VERSION = 1;`
  - `enum class mp_msg_kind : uint8_t { control = 1, screen = 2 };`
  - `std::vector<uint8_t> mp_wrap( mp_msg_kind kind, const std::vector<uint8_t> &body )`
  - `bool mp_unwrap( const std::vector<uint8_t> &payload, mp_msg_kind &kind, std::vector<uint8_t> &body )`
  - `struct mp_hello { int proto; std::string name; int width; int height; bool colors_256; }`
  - `struct mp_hello_ack { bool accepted; std::string reason; }`
  - `struct mp_resize { int width; int height; }`
  - `struct mp_cursor { int x; int y; bool visible; }`
  - `struct mp_control` — вариант: `kind` из `{ hello, hello_ack, resize, input, cursor, bye }`
    плюс поля соответствующих структур и `input_event event`
  - `std::string mp_control_to_json( const mp_control & )`
  - `bool mp_control_from_json( const std::string &, mp_control & )`

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_protocol_test.cpp`:

```cpp
#include "cata_catch.h"

#include <string>
#include <vector>

#include "input_enums.h"
#include "mp/mp_protocol.h"

TEST_CASE( "mp_wrap tags a body with its kind", "[mp]" )
{
    const std::vector<uint8_t> body = { 1, 2, 3 };
    const std::vector<uint8_t> wrapped = mp_wrap( mp_msg_kind::screen, body );

    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> got;
    REQUIRE( mp_unwrap( wrapped, kind, got ) );
    CHECK( kind == mp_msg_kind::screen );
    CHECK( got == body );
}

TEST_CASE( "mp_unwrap rejects an empty payload", "[mp]" )
{
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> got;
    CHECK_FALSE( mp_unwrap( {}, kind, got ) );
}

TEST_CASE( "mp_unwrap rejects an unknown kind byte", "[mp]" )
{
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> got;
    CHECK_FALSE( mp_unwrap( { 99, 1, 2 }, kind, got ) );
}

TEST_CASE( "mp_control round trips a hello", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::hello;
    sent.hello.proto = MP_PROTOCOL_VERSION;
    sent.hello.name = "serg";
    sent.hello.width = 120;
    sent.hello.height = 40;
    sent.hello.colors_256 = true;

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::hello );
    CHECK( got.hello.proto == MP_PROTOCOL_VERSION );
    CHECK( got.hello.name == "serg" );
    CHECK( got.hello.width == 120 );
    CHECK( got.hello.height == 40 );
    CHECK( got.hello.colors_256 );
}

TEST_CASE( "mp_control round trips a rejection with its reason", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::hello_ack;
    sent.hello_ack.accepted = false;
    sent.hello_ack.reason = "protocol version mismatch: server 1, client 2";

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::hello_ack );
    CHECK_FALSE( got.hello_ack.accepted );
    CHECK( got.hello_ack.reason == sent.hello_ack.reason );
}

TEST_CASE( "mp_control round trips a keyboard input event", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::input;
    sent.event.type = input_event_t::keyboard_code;
    sent.event.sequence = { 106 };
    sent.event.modifiers = { keymod_t::ctrl, keymod_t::shift };
    sent.event.text = "j";
    sent.event.edit = "";
    sent.event.edit_refresh = false;

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::input );
    CHECK( got.event.type == input_event_t::keyboard_code );
    CHECK( got.event.sequence == std::vector<int> { 106 } );
    CHECK( got.event.modifiers == std::set<keymod_t> { keymod_t::ctrl, keymod_t::shift } );
    CHECK( got.event.text == "j" );
    CHECK_FALSE( got.event.edit_refresh );
}

TEST_CASE( "mp_control round trips a mouse input event", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::input;
    sent.event.type = input_event_t::mouse;
    sent.event.sequence = { 0 };
    sent.event.mouse_pos = point( 37, 11 );

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.event.type == input_event_t::mouse );
    CHECK( got.event.mouse_pos == point( 37, 11 ) );
}

TEST_CASE( "mp_control round trips a resize and a cursor", "[mp]" )
{
    mp_control resize;
    resize.kind = mp_control_kind::resize;
    resize.resize.width = 90;
    resize.resize.height = 30;

    mp_control got_resize;
    REQUIRE( mp_control_from_json( mp_control_to_json( resize ), got_resize ) );
    CHECK( got_resize.resize.width == 90 );
    CHECK( got_resize.resize.height == 30 );

    mp_control cursor;
    cursor.kind = mp_control_kind::cursor;
    cursor.cursor.x = 4;
    cursor.cursor.y = 5;
    cursor.cursor.visible = true;

    mp_control got_cursor;
    REQUIRE( mp_control_from_json( mp_control_to_json( cursor ), got_cursor ) );
    CHECK( got_cursor.cursor.x == 4 );
    CHECK( got_cursor.cursor.y == 5 );
    CHECK( got_cursor.cursor.visible );
}

TEST_CASE( "mp_control_from_json rejects malformed input", "[mp]" )
{
    mp_control got;
    CHECK_FALSE( mp_control_from_json( "not json at all", got ) );
    CHECK_FALSE( mp_control_from_json( "{\"t\":\"nonsense\"}", got ) );
}
```

- [ ] **Step 2: Прогнать тест и убедиться, что он падает**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: `mp/mp_protocol.h: No such file or directory`.

- [ ] **Step 3: Написать заголовок**

Создать `src/mp/mp_protocol.h`:

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_PROTOCOL_H
#define CATA_SRC_MP_MP_PROTOCOL_H

#include <cstdint>
#include <string>
#include <vector>

#include "input_enums.h"

// Version of the wire protocol. Sent in the client's first message; a
// mismatch is refused with an explanation rather than tolerated, because the
// failure mode of a silent mismatch is a garbled screen.
constexpr int MP_PROTOCOL_VERSION = 1;

// Every framed payload starts with one kind byte. Control traffic is JSON, so
// it can be read in a log by eye; screen traffic is the compact binary of
// mp_screen_codec, because it is the hot path.
enum class mp_msg_kind : uint8_t {
    control = 1,
    screen = 2,
};

std::vector<uint8_t> mp_wrap( mp_msg_kind kind, const std::vector<uint8_t> &body );
bool mp_unwrap( const std::vector<uint8_t> &payload, mp_msg_kind &kind,
                std::vector<uint8_t> &body );

enum class mp_control_kind {
    hello,
    hello_ack,
    resize,
    input,
    cursor,
    bye,
};

struct mp_hello {
    int proto = 0;
    std::string name;
    int width = 0;
    int height = 0;
    bool colors_256 = false;
};

struct mp_hello_ack {
    bool accepted = false;
    std::string reason;
};

struct mp_resize {
    int width = 0;
    int height = 0;
};

struct mp_cursor {
    int x = 0;
    int y = 0;
    bool visible = false;
};

// One control message. Only the member matching `kind` is meaningful; this is
// a plain struct rather than a variant so that adding a message type later is
// a local edit.
struct mp_control {
    mp_control_kind kind = mp_control_kind::bye;
    mp_hello hello;
    mp_hello_ack hello_ack;
    mp_resize resize;
    mp_cursor cursor;
    input_event event;
    std::string bye_reason;
};

std::string mp_control_to_json( const mp_control &msg );
bool mp_control_from_json( const std::string &text, mp_control &out );

#endif // CATA_SRC_MP_MP_PROTOCOL_H
```

- [ ] **Step 4: Написать реализацию**

Создать `src/mp/mp_protocol.cpp`. Использовать существующие `JsonOut` (`src/json.h`) и
`JsonValue`; парсинг оборачивать в `try`/`catch( const JsonError & )`, потому что
`JsonValue` бросает на некорректном вводе, а сеть — источник некорректного ввода по
определению.

```cpp
#include "mp/mp_protocol.h"

#include <sstream>

#include "json.h"
#include "point.h"

std::vector<uint8_t> mp_wrap( const mp_msg_kind kind, const std::vector<uint8_t> &body )
{
    std::vector<uint8_t> out;
    out.reserve( body.size() + 1 );
    out.push_back( static_cast<uint8_t>( kind ) );
    out.insert( out.end(), body.begin(), body.end() );
    return out;
}

bool mp_unwrap( const std::vector<uint8_t> &payload, mp_msg_kind &kind,
                std::vector<uint8_t> &body )
{
    if( payload.empty() ) {
        return false;
    }
    switch( payload[0] ) {
        case static_cast<uint8_t>( mp_msg_kind::control ):
            kind = mp_msg_kind::control;
            break;
        case static_cast<uint8_t>( mp_msg_kind::screen ):
            kind = mp_msg_kind::screen;
            break;
        default:
            return false;
    }
    body.assign( payload.begin() + 1, payload.end() );
    return true;
}

namespace
{

const char *kind_to_string( const mp_control_kind kind )
{
    switch( kind ) {
        case mp_control_kind::hello:
            return "hello";
        case mp_control_kind::hello_ack:
            return "hello_ack";
        case mp_control_kind::resize:
            return "resize";
        case mp_control_kind::input:
            return "input";
        case mp_control_kind::cursor:
            return "cursor";
        case mp_control_kind::bye:
            return "bye";
    }
    return "bye";
}

bool string_to_kind( const std::string &text, mp_control_kind &out )
{
    if( text == "hello" ) {
        out = mp_control_kind::hello;
    } else if( text == "hello_ack" ) {
        out = mp_control_kind::hello_ack;
    } else if( text == "resize" ) {
        out = mp_control_kind::resize;
    } else if( text == "input" ) {
        out = mp_control_kind::input;
    } else if( text == "cursor" ) {
        out = mp_control_kind::cursor;
    } else if( text == "bye" ) {
        out = mp_control_kind::bye;
    } else {
        return false;
    }
    return true;
}

void write_event( JsonOut &jo, const input_event &ev )
{
    jo.member( "etype", static_cast<int>( ev.type ) );
    jo.member( "seq" );
    jo.write( ev.sequence );
    jo.member( "mods" );
    jo.start_array();
    for( const keymod_t m : ev.modifiers ) {
        jo.write( static_cast<int>( m ) );
    }
    jo.end_array();
    jo.member( "mx", ev.mouse_pos.x );
    jo.member( "my", ev.mouse_pos.y );
    jo.member( "text", ev.text );
    jo.member( "edit", ev.edit );
    jo.member( "edit_refresh", ev.edit_refresh );
}

void read_event( const JsonObject &obj, input_event &ev )
{
    ev.type = static_cast<input_event_t>( obj.get_int( "etype" ) );
    ev.sequence.clear();
    for( const int key : obj.get_int_array( "seq" ) ) {
        ev.sequence.push_back( key );
    }
    ev.modifiers.clear();
    for( const int mod : obj.get_int_array( "mods" ) ) {
        ev.modifiers.insert( static_cast<keymod_t>( mod ) );
    }
    ev.mouse_pos = point( obj.get_int( "mx" ), obj.get_int( "my" ) );
    ev.text = obj.get_string( "text" );
    ev.edit = obj.get_string( "edit" );
    ev.edit_refresh = obj.get_bool( "edit_refresh" );
}

} // namespace

std::string mp_control_to_json( const mp_control &msg )
{
    std::ostringstream stream;
    JsonOut jo( stream );
    jo.start_object();
    jo.member( "t", std::string( kind_to_string( msg.kind ) ) );
    switch( msg.kind ) {
        case mp_control_kind::hello:
            jo.member( "proto", msg.hello.proto );
            jo.member( "name", msg.hello.name );
            jo.member( "w", msg.hello.width );
            jo.member( "h", msg.hello.height );
            jo.member( "c256", msg.hello.colors_256 );
            break;
        case mp_control_kind::hello_ack:
            jo.member( "ok", msg.hello_ack.accepted );
            jo.member( "reason", msg.hello_ack.reason );
            break;
        case mp_control_kind::resize:
            jo.member( "w", msg.resize.width );
            jo.member( "h", msg.resize.height );
            break;
        case mp_control_kind::input:
            write_event( jo, msg.event );
            break;
        case mp_control_kind::cursor:
            jo.member( "x", msg.cursor.x );
            jo.member( "y", msg.cursor.y );
            jo.member( "vis", msg.cursor.visible );
            break;
        case mp_control_kind::bye:
            jo.member( "reason", msg.bye_reason );
            break;
    }
    jo.end_object();
    return stream.str();
}

bool mp_control_from_json( const std::string &text, mp_control &out )
{
    try {
        std::istringstream stream( text );
        JsonValue value = json_loader::from_string( text );
        JsonObject obj = value.get_object();
        if( !obj.has_string( "t" ) ) {
            return false;
        }
        mp_control msg;
        if( !string_to_kind( obj.get_string( "t" ), msg.kind ) ) {
            return false;
        }
        switch( msg.kind ) {
            case mp_control_kind::hello:
                msg.hello.proto = obj.get_int( "proto" );
                msg.hello.name = obj.get_string( "name" );
                msg.hello.width = obj.get_int( "w" );
                msg.hello.height = obj.get_int( "h" );
                msg.hello.colors_256 = obj.get_bool( "c256" );
                break;
            case mp_control_kind::hello_ack:
                msg.hello_ack.accepted = obj.get_bool( "ok" );
                msg.hello_ack.reason = obj.get_string( "reason" );
                break;
            case mp_control_kind::resize:
                msg.resize.width = obj.get_int( "w" );
                msg.resize.height = obj.get_int( "h" );
                break;
            case mp_control_kind::input:
                read_event( obj, msg.event );
                break;
            case mp_control_kind::cursor:
                msg.cursor.x = obj.get_int( "x" );
                msg.cursor.y = obj.get_int( "y" );
                msg.cursor.visible = obj.get_bool( "vis" );
                break;
            case mp_control_kind::bye:
                msg.bye_reason = obj.get_string( "reason" );
                break;
        }
        out = msg;
        return true;
    } catch( const JsonError & ) {
        return false;
    }
}
```

- [ ] **Step 5: Согласовать API работы с JSON с тем, что есть в проекте**

Точные имена (`json_loader::from_string`, `JsonObject::get_int_array`, `JsonOut::write`
для `std::vector<int>`) проверить по `src/json.h` и `src/json_loader.h` и поправить, если
не совпало. Ошибок компиляции быть не должно: параллельные вызовы уже используются в
`src/savegame.cpp`.

```bash
grep -n "from_string\|get_int_array\|has_string" src/json_loader.h src/json.h | head -20
```

- [ ] **Step 6: Прогнать тесты**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Ожидается: все тест-кейсы из задач 1–3 проходят.

- [ ] **Step 7: Коммит**

```bash
git add src/mp/mp_protocol.h src/mp/mp_protocol.cpp tests/mp_protocol_test.cpp
git commit -m "feat(mp): add wire protocol control messages"
```

---

### Task 4: Транспорт и loopback-реализация

**Files:**
- Create: `src/mp/mp_transport.h`, `src/mp/mp_loopback_transport.h`, `src/mp/mp_loopback_transport.cpp`
- Test: `tests/mp_loopback_transport_test.cpp`

**Interfaces:**
- Consumes: ничего (loopback не кадрирует — он передаёт готовые сообщения).
- Produces:
  - `class mp_transport` с `send( std::vector<uint8_t> )`, `recv( int timeout_ms )`,
    `connected()`, `close()`
  - `struct mp_loopback_pair { std::shared_ptr<mp_transport> a, b; }`
  - `mp_loopback_pair mp_make_loopback_pair( int simulated_latency_ms = 0 )`

**Почему инжекция задержки.** Спек §10 требует её явно: без неё неизбежно напишется код,
работающий только при нулевом RTT.

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_loopback_transport_test.cpp`:

```cpp
#include "cata_catch.h"

#include <chrono>
#include <cstdint>
#include <vector>

#include "mp/mp_loopback_transport.h"
#include "mp/mp_transport.h"

TEST_CASE( "loopback transport delivers messages both ways", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();

    pair.a->send( { 1, 2, 3 } );
    const std::optional<std::vector<uint8_t>> at_b = pair.b->recv( 0 );
    REQUIRE( at_b.has_value() );
    CHECK( *at_b == std::vector<uint8_t> { 1, 2, 3 } );

    pair.b->send( { 9 } );
    const std::optional<std::vector<uint8_t>> at_a = pair.a->recv( 0 );
    REQUIRE( at_a.has_value() );
    CHECK( *at_a == std::vector<uint8_t> { 9 } );
}

TEST_CASE( "loopback transport preserves message order", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    pair.a->send( { 1 } );
    pair.a->send( { 2 } );
    pair.a->send( { 3 } );

    CHECK( pair.b->recv( 0 ) == std::vector<uint8_t> { 1 } );
    CHECK( pair.b->recv( 0 ) == std::vector<uint8_t> { 2 } );
    CHECK( pair.b->recv( 0 ) == std::vector<uint8_t> { 3 } );
    CHECK_FALSE( pair.b->recv( 0 ).has_value() );
}

TEST_CASE( "loopback transport reports a closed peer", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    REQUIRE( pair.a->connected() );
    pair.b->close();
    CHECK_FALSE( pair.a->connected() );
}

TEST_CASE( "loopback transport honours simulated latency", "[mp]" )
{
    // 20 ms each way. A message sent now must not be readable immediately,
    // and must be readable when we are willing to wait long enough.
    mp_loopback_pair pair = mp_make_loopback_pair( 20 );
    pair.a->send( { 42 } );

    CHECK_FALSE( pair.b->recv( 0 ).has_value() );

    const std::optional<std::vector<uint8_t>> got = pair.b->recv( 200 );
    REQUIRE( got.has_value() );
    CHECK( *got == std::vector<uint8_t> { 42 } );
}
```

- [ ] **Step 2: Прогнать тест и убедиться, что он падает**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: `mp/mp_loopback_transport.h: No such file or directory`.

- [ ] **Step 3: Написать `mp_transport.h`**

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_TRANSPORT_H
#define CATA_SRC_MP_MP_TRANSPORT_H

#include <cstdint>
#include <optional>
#include <vector>

// A bidirectional, ordered, reliable message channel.
//
// Messages are whole payloads: framing, if the underlying medium needs it, is
// the implementation's business. Two implementations exist -- TCP for real
// play and an in-memory pair for tests.
class mp_transport
{
    public:
        virtual ~mp_transport() = default;

        // Queues a message for delivery. Never blocks; a peer that cannot keep
        // up causes the outgoing buffer to grow, which the caller handles.
        virtual void send( std::vector<uint8_t> payload ) = 0;

        // Returns the next message. `timeout_ms` of 0 polls, a negative value
        // blocks until a message arrives or the peer goes away.
        virtual std::optional<std::vector<uint8_t>> recv( int timeout_ms ) = 0;

        virtual bool connected() const = 0;
        virtual void close() = 0;
};

#endif // CATA_SRC_MP_MP_TRANSPORT_H
```

- [ ] **Step 4: Написать loopback-реализацию**

Создать `src/mp/mp_loopback_transport.h` и `.cpp`. Реализация — две очереди
`std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<uint8_t>>>`,
общая структура под `std::shared_ptr`, флаг закрытия. `recv` при задержке отдаёт сообщение
только когда `now >= ready_at`; при положительном таймауте — спит до наступления этого
момента, но не дольше таймаута. Мьютекс нужен, потому что в Ф3 те же транспорты будут
использоваться из разных потоков; здесь он не мешает.

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_LOOPBACK_TRANSPORT_H
#define CATA_SRC_MP_MP_LOOPBACK_TRANSPORT_H

#include <memory>

#include "mp/mp_transport.h"

struct mp_loopback_pair {
    std::shared_ptr<mp_transport> a;
    std::shared_ptr<mp_transport> b;
};

// Creates two transports wired to each other in memory. `simulated_latency_ms`
// delays delivery in each direction, so tests can catch code that only works
// at zero round-trip time.
mp_loopback_pair mp_make_loopback_pair( int simulated_latency_ms = 0 );

#endif // CATA_SRC_MP_MP_LOOPBACK_TRANSPORT_H
```

```cpp
#include "mp/mp_loopback_transport.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace
{

using clock = std::chrono::steady_clock;

struct loopback_channel {
    std::mutex mutex;
    std::deque<std::pair<clock::time_point, std::vector<uint8_t>>> queue;
    bool closed = false;
};

class loopback_transport : public mp_transport
{
    public:
        loopback_transport( std::shared_ptr<loopback_channel> outgoing,
                            std::shared_ptr<loopback_channel> incoming,
                            const int latency_ms )
            : outgoing_( std::move( outgoing ) ), incoming_( std::move( incoming ) ),
              latency_ms_( latency_ms ) {}

        void send( std::vector<uint8_t> payload ) override {
            std::lock_guard<std::mutex> lock( outgoing_->mutex );
            if( outgoing_->closed ) {
                return;
            }
            outgoing_->queue.emplace_back( clock::now() + std::chrono::milliseconds( latency_ms_ ),
                                           std::move( payload ) );
        }

        std::optional<std::vector<uint8_t>> recv( const int timeout_ms ) override {
            const clock::time_point deadline = timeout_ms < 0
                                               ? clock::time_point::max()
                                               : clock::now() + std::chrono::milliseconds( timeout_ms );
            while( true ) {
                {
                    std::lock_guard<std::mutex> lock( incoming_->mutex );
                    if( !incoming_->queue.empty() && incoming_->queue.front().first <= clock::now() ) {
                        std::vector<uint8_t> payload = std::move( incoming_->queue.front().second );
                        incoming_->queue.pop_front();
                        return payload;
                    }
                    if( incoming_->closed && incoming_->queue.empty() ) {
                        return std::nullopt;
                    }
                }
                if( clock::now() >= deadline ) {
                    return std::nullopt;
                }
                // A one millisecond poll is coarse, but this transport only
                // ever runs in tests, where responsiveness does not matter.
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        }

        bool connected() const override {
            std::lock_guard<std::mutex> lock( incoming_->mutex );
            return !incoming_->closed;
        }

        void close() override {
            {
                std::lock_guard<std::mutex> lock( outgoing_->mutex );
                outgoing_->closed = true;
            }
            std::lock_guard<std::mutex> lock( incoming_->mutex );
            incoming_->closed = true;
        }

    private:
        std::shared_ptr<loopback_channel> outgoing_;
        std::shared_ptr<loopback_channel> incoming_;
        int latency_ms_ = 0;
};

} // namespace

mp_loopback_pair mp_make_loopback_pair( const int simulated_latency_ms )
{
    auto a_to_b = std::make_shared<loopback_channel>();
    auto b_to_a = std::make_shared<loopback_channel>();
    mp_loopback_pair pair;
    pair.a = std::make_shared<loopback_transport>( a_to_b, b_to_a, simulated_latency_ms );
    pair.b = std::make_shared<loopback_transport>( b_to_a, a_to_b, simulated_latency_ms );
    return pair;
}
```

Примечание: `connected()` объявлен `const`, а мьютекс блокируется — поле `mutex` в
`loopback_channel` придётся объявить `mutable`, либо снять `const` с метода. Выбрать
`mutable` и не менять интерфейс.

- [ ] **Step 5: Прогнать тесты**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Ожидается: все тесты задач 1–4 проходят, включая тест с задержкой.

- [ ] **Step 6: Коммит**

```bash
git add src/mp/mp_transport.h src/mp/mp_loopback_transport.h \
        src/mp/mp_loopback_transport.cpp tests/mp_loopback_transport_test.cpp
git commit -m "feat(mp): add transport interface and in-memory loopback"
```

---

### Task 5: Цель сборки сервера и пустой бэкенд

**Files:**
- Create: `src/mp/mp_curses_server.cpp`
- Modify: `src/cursesport.h:5-7`, `src/ncurses_def.cpp:1`, `Makefile:1105`
- Test: сборка (см. шаги)

**Interfaces:**
- Consumes: ничего.
- Produces: собирающаяся цель `cataclysm-server`; все бэкендные символы определены заглушками.

**Зачем отдельная задача.** Сборочный каркас — единственная часть, которую нельзя проверить
юнит-тестом, и единственная, которая трогает чужие файлы. Она изолирована, чтобы ревьюер мог
принять или отклонить её отдельно от логики.

- [ ] **Step 1: Найти все символы, которые обязан определить бэкенд**

```bash
grep -n "^[a-zA-Z_].*(" src/wincurse.cpp | grep -v "^\s" | grep -v "static" | head -40
```

Список (проверить, что он совпадает; расхождение — повод остановиться):
`catacurses::init_interface`, `catacurses::endwin`, `catacurses::supports_256_colors`,
`cata_cursesport::curses_drawwindow`, `input_manager::pump_events`,
`input_manager::get_input_event`, `input_manager::set_timeout`,
`input_context::get_coordinates`, `gamepad_available`, `handle_resize`, `resize_term`,
`projected_window_width`, `projected_window_height`, `get_terminal_width`,
`get_terminal_height`, `refresh_display`, `drain_renderer_recovery`, `refresh_mouse_config`,
`set_title`, `get_scaling_factor`.

- [ ] **Step 2: Открыть путь к буферной реализации curses**

`src/cursesport.h:5-7` сейчас:

```cpp
#if defined(IMTUI) || !(defined(TILES) || defined(WIN32))
#   define TUI
#endif
```

Заменить на:

```cpp
// MP_SERVER wants the in-memory curses of cursesport.cpp -- windows that are
// plain cell buffers -- rather than a terminal, so it must not take the TUI
// path even though it defines IMTUI to render imgui through imtui's text
// backend. See docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md 5.1.
#if !defined(MP_SERVER) && (defined(IMTUI) || !(defined(TILES) || defined(WIN32)))
#   define TUI
#endif
```

`src/ncurses_def.cpp:1` сейчас `#if !(defined(TILES))`. Заменить на
`#if !(defined(TILES) || defined(MP_SERVER))`.

- [ ] **Step 3: Проверить, что `TUI` больше нигде не значит чего-то другого**

```bash
grep -rn "defined(TUI)\|ifdef TUI\|ifndef TUI" src/ | grep -v third-party
```

Ожидается: вхождения только в `cursesport.h`, `cursesport.cpp`, `wincurse.cpp`. Если есть
другие — прочитать каждое и убедиться, что новая ветка их не ломает.

- [ ] **Step 4: Добавить флаг и цель в Makefile**

Рядом с блоком `ifeq ($(MULTIPLAYER), 1)` (`Makefile:1105`) добавить:

```makefile
ifeq ($(MP_SERVER), 1)
  # The server links the in-memory curses of cursesport.cpp plus the backend in
  # src/mp/mp_curses_server.cpp, and renders imgui through imtui's text backend.
  # It implies MULTIPLAYER and never links SDL or ncurses.
  MULTIPLAYER = 1
  DEFINES += -DMP_SERVER -DIMTUI
  IMGUI_SOURCES += $(IMTUI_DIR)/imtui-impl-text.cpp
  TARGET_NAME = cataclysm-server
endif
```

Точное имя переменной цели (`TARGET_NAME` или другое) взять из Makefile, найдя, как
называется цель для обычной сборки:

```bash
grep -n "TARGET_NAME\|^TARGET " Makefile | head
```

- [ ] **Step 5: Написать пустой бэкенд**

Создать `src/mp/mp_curses_server.cpp` с заглушками всех символов из шага 1. Каждая функция
либо возвращает безобидное значение, либо ничего не делает; `get_input_event` возвращает
событие типа `error`. Пример нескольких, остальные по образцу:

```cpp
#if defined(MP_SERVER)

#include "cata_utility.h"
#include "cursesdef.h"
#include "cursesport.h"
#include "input.h"
#include "output.h"
#include "point.h"

// Stub backend. Task 6 replaces every body here with the real screen and input
// path; this file exists first so that the build target can be reviewed and
// landed on its own.

void catacurses::init_interface()
{
}

void catacurses::endwin()
{
}

bool catacurses::supports_256_colors()
{
    return true;
}

void cata_cursesport::curses_drawwindow( const catacurses::window &/*win*/ )
{
}

void input_manager::pump_events()
{
}

input_event input_manager::get_input_event( const keyboard_mode /*mode*/ )
{
    return input_event();
}

void input_manager::set_timeout( const int /*delay*/ )
{
}

bool gamepad_available()
{
    return false;
}

int get_terminal_width()
{
    return 80;
}

int get_terminal_height()
{
    return 24;
}

// ... the remaining symbols from Step 1, in the same style.

#endif // MP_SERVER
```

- [ ] **Step 6: Собрать серверную цель**

```bash
make -j$(nproc) CCACHE=1 TESTS=0 MP_SERVER=1 BUILD_PREFIX=mpserver-
```

Ожидается: линковка проходит. Типичные ошибки на этом шаге — недостающие символы
(добавить в бэкенд) и дублирующиеся (какой-то файл не отключился guard'ом; прочитать его
условие компиляции). Итерировать до чистой сборки.

- [ ] **Step 7: Убедиться, что обычные сборки не сломались**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir=/tmp/cata-mp-test/
```

Ожидается: тесты зелёные, число тест-кейсов выросло ровно на добавленные `[mp]`.

- [ ] **Step 8: Коммит**

```bash
git add src/mp/mp_curses_server.cpp src/cursesport.h src/ncurses_def.cpp Makefile
git commit -m "build: add cataclysm-server target with a stub curses backend"
```

---

### Task 6: Серверный бэкенд — экран

**Files:**
- Create: `src/mp/mp_server_screen.h`, `src/mp/mp_server_screen.cpp`
- Modify: `src/mp/mp_curses_server.cpp`
- Test: `tests/mp_server_session_test.cpp` (новый, только при `MP_SERVER`)

**Interfaces:**
- Consumes: `mp_screen_grid`, `mp_screen_encode`, `mp_wrap`, `mp_transport`.
- Produces:
  - `mp_screen_grid &mp_server_grid()`
  - `void mp_server_set_transport( std::shared_ptr<mp_transport> )`
  - `void mp_server_set_terminal_size( int w, int h )`
  - `void mp_server_flush_screen()` — кодирует грязные строки, отправляет, чистит флаги
  - `void mp_server_blit_window( const catacurses::window &win )`

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_server_session_test.cpp`:

```cpp
#if defined(MP_SERVER)

#include "cata_catch.h"

#include <memory>
#include <vector>

#include "cursesdef.h"
#include "mp/mp_cell.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_server_screen.h"
#include "point.h"

TEST_CASE( "server blits a curses window into the screen grid", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    catacurses::window win = catacurses::newwin( 3, 10, point( 2, 1 ) );
    catacurses::mvwprintw( win, point( 0, 0 ), "hello" );
    catacurses::wrefresh( win );

    CHECK( mp_server_grid().at( 2, 1 ).ch == "h" );
    CHECK( mp_server_grid().at( 6, 1 ).ch == "o" );
}

TEST_CASE( "server sends only what changed", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    // First flush carries the whole screen.
    mp_server_flush_screen();
    const std::optional<std::vector<uint8_t>> full = pair.b->recv( 0 );
    REQUIRE( full.has_value() );

    // Nothing changed: nothing is sent.
    mp_server_flush_screen();
    CHECK_FALSE( pair.b->recv( 0 ).has_value() );

    // One window redraw sends a much smaller message than the full screen.
    catacurses::window win = catacurses::newwin( 1, 5, point( 0, 0 ) );
    catacurses::mvwprintw( win, point( 0, 0 ), "abcde" );
    catacurses::wrefresh( win );
    mp_server_flush_screen();

    const std::optional<std::vector<uint8_t>> delta = pair.b->recv( 0 );
    REQUIRE( delta.has_value() );
    CHECK( delta->size() < full->size() / 4 );
}

TEST_CASE( "a client can reconstruct the server screen from the wire", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    catacurses::window win = catacurses::newwin( 2, 20, point( 5, 3 ) );
    catacurses::mvwprintw( win, point( 0, 0 ), "reconstruct me" );
    catacurses::wrefresh( win );
    mp_server_flush_screen();

    mp_screen_grid client( 40, 10 );
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        if( kind == mp_msg_kind::screen ) {
            REQUIRE( mp_screen_apply( client, body ) );
        }
    }

    for( int y = 0; y < 10; ++y ) {
        for( int x = 0; x < 40; ++x ) {
            REQUIRE( client.at( x, y ) == mp_server_grid().at( x, y ) );
        }
    }
}

#endif // MP_SERVER
```

- [ ] **Step 2: Прогнать тест в серверной сборке и убедиться, что он падает**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
```

Ожидается: `mp/mp_server_screen.h: No such file or directory`.

- [ ] **Step 3: Написать `mp_server_screen.h`**

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_SCREEN_H
#define CATA_SRC_MP_MP_SERVER_SCREEN_H

#include <memory>

#include "mp/mp_cell.h"

namespace catacurses
{
class window;
} // namespace catacurses

class mp_transport;

// The server's composite screen for the connected player.
//
// Every curses window the game draws is blitted here instead of to a terminal;
// the accumulated dirty rows are shipped to the client at exactly one point --
// when the game is about to block waiting for that player's next keystroke.
// Flushing there rather than on every refresh collapses the dozen redraws a
// single action provokes into one message.

mp_screen_grid &mp_server_grid();

void mp_server_set_terminal_size( int width, int height );
void mp_server_set_transport( std::shared_ptr<mp_transport> transport );

// Copies the touched lines of `win` into the grid and clears their touched
// flags, mirroring what a drawing backend would do.
void mp_server_blit_window( const catacurses::window &win );

// Encodes and sends the dirty rows, then clears them. Does nothing when
// nothing changed or when no transport is attached.
void mp_server_flush_screen();

#endif // CATA_SRC_MP_MP_SERVER_SCREEN_H
```

- [ ] **Step 4: Написать `mp_server_screen.cpp`**

Ключевой цикл повторяет то, что делает `curses_drawwindow` в `src/sdltiles.cpp`: идти по
строкам окна, пропускать нетронутые, копировать ячейки со смещением на `win->pos`, учитывать
пустую `ch` как продолжение двухколоночного символа.

```cpp
#include "mp/mp_server_screen.h"

#include <memory>
#include <utility>

#include "cursesdef.h"
#include "cursesport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_transport.h"

namespace
{
mp_screen_grid grid( 80, 24 );
std::shared_ptr<mp_transport> transport;
} // namespace

mp_screen_grid &mp_server_grid()
{
    return grid;
}

void mp_server_set_terminal_size( const int width, const int height )
{
    grid.resize( width, height );
}

void mp_server_set_transport( std::shared_ptr<mp_transport> t )
{
    transport = std::move( t );
}

void mp_server_blit_window( const catacurses::window &win_ )
{
    cata_cursesport::WINDOW *const win = win_.get<cata_cursesport::WINDOW>();
    if( win == nullptr ) {
        return;
    }
    for( int j = 0; j < win->height; ++j ) {
        if( !win->line[j].touched ) {
            continue;
        }
        win->line[j].touched = false;
        for( int i = 0; i < win->width; ++i ) {
            const cata_cursesport::cursecell &cell = win->line[j].chars[i];
            if( cell.ch.empty() ) {
                // Second column of a double-width character: the client keeps
                // an empty glyph there, exactly as cursesport does.
                mp_cell blank;
                blank.ch.clear();
                blank.fg = static_cast<uint8_t>( cell.FG );
                blank.bg = static_cast<uint8_t>( cell.BG );
                grid.set( win->pos.x + i, win->pos.y + j, blank );
                continue;
            }
            mp_cell out;
            out.ch = cell.ch;
            out.fg = static_cast<uint8_t>( cell.FG );
            out.bg = static_cast<uint8_t>( cell.BG );
            grid.set( win->pos.x + i, win->pos.y + j, out );
        }
    }
}

void mp_server_flush_screen()
{
    if( !transport || !grid.any_dirty() ) {
        return;
    }
    transport->send( mp_wrap( mp_msg_kind::screen, mp_screen_encode( grid ) ) );
    grid.clear_dirty();
}
```

- [ ] **Step 5: Подключить бэкенд к экрану**

В `src/mp/mp_curses_server.cpp` заменить заглушки:

```cpp
void cata_cursesport::curses_drawwindow( const catacurses::window &win )
{
    mp_server_blit_window( win );
}

void catacurses::init_interface()
{
    // The client told us its terminal size during the handshake; the game's
    // root window must match it exactly.
    catacurses::stdscr = catacurses::newwin( get_terminal_height(), get_terminal_width(),
                                             point::zero );
}

int get_terminal_width()
{
    return mp_server_grid().width();
}

int get_terminal_height()
{
    return mp_server_grid().height();
}

void refresh_display()
{
    // Deliberately empty: the screen goes out when the game blocks for input,
    // not on every refresh. See mp_server_screen.h.
}
```

Остальные заглушки (`drain_renderer_recovery`, `refresh_mouse_config`, `get_scaling_factor`,
`projected_window_width`, `projected_window_height`, `resize_term`) оставить пустыми до
Task 8.

- [ ] **Step 6: Прогнать тесты в серверной сборке**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
mpserver-tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Точный путь к тестовому бинарю в сборке с `BUILD_PREFIX` уточнить:

```bash
find . -name "cata_test" -newer Makefile | head
```

Ожидается: три новых тест-кейса проходят.

- [ ] **Step 7: Коммит**

```bash
git add src/mp/mp_server_screen.h src/mp/mp_server_screen.cpp \
        src/mp/mp_curses_server.cpp tests/mp_server_session_test.cpp
git commit -m "feat(mp): stream the server screen as dirty-row deltas"
```

---

### Task 7: Серверный бэкенд — ввод

**Files:**
- Modify: `src/mp/mp_curses_server.cpp`
- Create: `src/mp/mp_server_input.h`, `src/mp/mp_server_input.cpp`
- Test: `tests/mp_server_session_test.cpp` (дополнить)

**Interfaces:**
- Consumes: `mp_control`, `mp_control_from_json`, `mp_unwrap`, `mp_server_flush_screen`.
- Produces:
  - `input_event mp_server_wait_for_input( int timeout_ms )`
  - `void mp_server_pump( )` — неблокирующий разбор входящих сообщений
  - `void mp_server_set_cursor( int x, int y, bool visible )`

**Правило.** Именно здесь экран уходит клиенту: перед тем как заблокироваться, бэкенд
вызывает `mp_server_flush_screen()`. Это единственная точка сброса.

- [ ] **Step 1: Дописать падающий тест**

Добавить в `tests/mp_server_session_test.cpp`:

```cpp
TEST_CASE( "server flushes the screen before blocking for input", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );
    mp_server_flush_screen();
    while( pair.b->recv( 0 ) ) {
        // drain the initial full screen
    }

    catacurses::window win = catacurses::newwin( 1, 5, point( 0, 0 ) );
    catacurses::mvwprintw( win, point( 0, 0 ), "ready" );
    catacurses::wrefresh( win );

    // Queue a keystroke so the wait returns, then check that the screen went
    // out before the input was consumed.
    mp_control key;
    key.kind = mp_control_kind::input;
    key.event.type = input_event_t::keyboard_char;
    key.event.sequence = { 'y' };
    pair.b->send( mp_wrap( mp_msg_kind::control,
                           std::vector<uint8_t>( mp_control_to_json( key ).begin(),
                                   mp_control_to_json( key ).end() ) ) );

    const input_event got = mp_server_wait_for_input( 1000 );
    CHECK( got.type == input_event_t::keyboard_char );
    CHECK( got.sequence == std::vector<int> { 'y' } );

    bool saw_screen = false;
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        saw_screen = saw_screen || kind == mp_msg_kind::screen;
    }
    CHECK( saw_screen );
}

TEST_CASE( "server returns a timeout event when no input arrives", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    const input_event got = mp_server_wait_for_input( 10 );
    CHECK( got.type == input_event_t::timeout );
}

TEST_CASE( "server applies a resize from the client", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    mp_control resize;
    resize.kind = mp_control_kind::resize;
    resize.resize.width = 60;
    resize.resize.height = 20;
    const std::string json = mp_control_to_json( resize );
    pair.b->send( mp_wrap( mp_msg_kind::control,
                           std::vector<uint8_t>( json.begin(), json.end() ) ) );

    mp_server_pump();
    CHECK( mp_server_grid().width() == 60 );
    CHECK( mp_server_grid().height() == 20 );
}
```

- [ ] **Step 2: Прогнать и убедиться, что падает**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
```

Ожидается: `mp/mp_server_input.h: No such file or directory`.

- [ ] **Step 3: Написать `mp_server_input.h` и `.cpp`**

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_INPUT_H
#define CATA_SRC_MP_MP_SERVER_INPUT_H

#include "input_enums.h"

// Blocks until the connected player presses something, the timeout expires, or
// the connection drops. Flushes the pending screen first: this is the single
// point at which the player sees what the game drew.
//
// `timeout_ms` of a negative value blocks indefinitely.
input_event mp_server_wait_for_input( int timeout_ms );

// Consumes whatever has already arrived without blocking. Used by the game's
// interrupt checks; keyboard events seen here are buffered for the next
// mp_server_wait_for_input().
void mp_server_pump();

void mp_server_set_cursor( int x, int y, bool visible );

#endif // CATA_SRC_MP_MP_SERVER_INPUT_H
```

Реализация: очередь `std::deque<input_event>` буферизованных нажатий; общая функция
`handle_message()`, которая разбирает одно входящее сообщение — `input` кладёт в очередь,
`resize` вызывает `mp_server_set_terminal_size` плюс `handle_resize`, `bye` закрывает
транспорт. `mp_server_wait_for_input` сначала `mp_server_flush_screen()`, потом отдаёт
буферизованное, потом ждёт на `transport->recv`.

- [ ] **Step 4: Подключить к бэкенду**

В `src/mp/mp_curses_server.cpp`:

```cpp
input_event input_manager::get_input_event( const keyboard_mode /*mode*/ )
{
    return mp_server_wait_for_input( input_timeout_ms );
}

void input_manager::pump_events()
{
    mp_server_pump();
}

void input_manager::set_timeout( const int delay )
{
    // Mirrors the ncurses backend: a negative delay means "block forever".
    input_timeout_ms = delay;
}

void catacurses::curs_set( const int visibility )
{
    mp_server_set_cursor( last_cursor_x, last_cursor_y, visibility != 0 );
}
```

Проверить, определяет ли `cursesport.cpp` уже `curs_set` — если да (`src/cursesport.cpp:485`),
то переопределять его в бэкенде нельзя, и позицию курсора надо брать из
`catacurses::getcurx/getcury( stdscr )` в момент сброса экрана. Выбрать второй путь.

- [ ] **Step 5: Прогнать тесты**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
mpserver-tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Ожидается: шесть тест-кейсов сессии проходят.

- [ ] **Step 6: Коммит**

```bash
git add src/mp/mp_server_input.h src/mp/mp_server_input.cpp src/mp/mp_curses_server.cpp \
        tests/mp_server_session_test.cpp
git commit -m "feat(mp): read player input from the transport on the server"
```

---

### Task 8: imgui через imtui

**Files:**
- Create: `src/mp/mp_imgui_server.cpp`
- Modify: `src/cata_imgui.cpp`
- Test: `tests/mp_server_session_test.cpp` (дополнить)

**Interfaces:**
- Consumes: `mp_server_grid()`.
- Produces: `void mp_imgui_draw_screen()` — выкладывает `ImTui::TScreen` в буфер сервера.

**Зачем.** Крафт, инвентарь, попапы и ещё шестнадцать файлов рисуются через imgui. В
текстовом режиме проект уже рендерит его через вендоренный `imtui`: `imtui-impl-text.cpp`
кладёт кадр в `ImTui::TScreen`, а `imtui-impl-ncurses.cpp` выкладывает этот буфер в curses
(`src/third-party/imtui/imtui-impl-ncurses.cpp:428`). Серверу нужен третий выкладыватель —
в наш буфер ячеек. Без него все imgui-интерфейсы у клиента будут пустыми.

- [ ] **Step 1: Прочитать, как ncurses-выкладыватель добирается до буфера**

```bash
sed -n '425,480p' src/third-party/imtui/imtui-impl-ncurses.cpp
```

Обратить внимание: `ImTui::ImplImtui_Data *bd = ImTui::ImTui_Impl_GetBackendData();` и
`ImTui::TScreen &screen = bd->Screen;`. Цвета `TCell::fg`/`TCell::bg` — индексы ansi256,
`TCell::ch` — `uint32_t` кодовая точка, `TCell::chwidth` — ширина в колонках.

- [ ] **Step 2: Написать падающий тест**

Добавить в `tests/mp_server_session_test.cpp`:

```cpp
TEST_CASE( "imgui frames reach the server screen grid", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    // Paint a recognisable pattern into the imtui screen and lay it out.
    mp_test_fill_imtui_screen( 40, 10, 'Z', 3, 1 );
    mp_imgui_draw_screen();

    CHECK( mp_server_grid().at( 0, 0 ).ch == "Z" );
    CHECK( mp_server_grid().at( 39, 9 ).ch == "Z" );
    CHECK( mp_server_grid().at( 5, 5 ).fg == 3 );
    CHECK( mp_server_grid().at( 5, 5 ).bg == 1 );
}
```

`mp_test_fill_imtui_screen` объявить в `src/mp/mp_imgui_server.cpp` и в заголовке
`src/mp/mp_imgui_server.h`; это тестовый хук, наполняющий `ImTui::TScreen` заданным
символом и цветами, чтобы не поднимать в тесте всю игру.

- [ ] **Step 3: Написать выкладыватель**

Создать `src/mp/mp_imgui_server.cpp` (весь файл под `#if defined(MP_SERVER)`). Цикл
повторяет `ImTui_ImplNcurses_DrawScreen`, но пишет в `mp_server_grid()`:

```cpp
void mp_imgui_draw_screen()
{
    ImTui::ImplImtui_Data *bd = ImTui::ImTui_Impl_GetBackendData();
    if( bd == nullptr ) {
        return;
    }
    ImTui::TScreen &screen = bd->Screen;
    mp_screen_grid &grid = mp_server_grid();

    for( int y = 0; y < screen.ny && y < grid.height(); ++y ) {
        for( int x = 0; x < screen.nx && x < grid.width(); ) {
            const ImTui::TCell &cell = screen.data[y * screen.nx + x];
            mp_cell out;
            out.ch = utf32_to_utf8( cell.ch );
            out.fg = cell.fg;
            out.bg = cell.bg;
            grid.set( x, y, out );
            // A double-width glyph occupies the next column with an empty
            // continuation cell, matching how cursesport stores wide text.
            for( int k = 1; k < cell.chwidth && x + k < grid.width(); ++k ) {
                mp_cell cont;
                cont.ch.clear();
                cont.fg = cell.fg;
                cont.bg = cell.bg;
                grid.set( x + k, y, cont );
            }
            x += cell.chwidth > 0 ? cell.chwidth : 1;
        }
    }
}
```

Функцию `utf32_to_utf8` взять существующую из `src/catacharset.h` (проверить точное имя:
`utf32_to_utf8` или `utf8_wrapper`), не писать свою.

```bash
grep -n "utf32_to_utf8\|std::string utf" src/catacharset.h
```

- [ ] **Step 4: Развести бэкенды в `cata_imgui.cpp`**

В `src/cata_imgui.cpp` вызовы `ImTui_ImplNcurses_Init/Shutdown/NewFrame/DrawScreen/
UploadColorPair/SetAllocedPairCount` (строки 105, 119, 133, 157, 167, 173) обернуть так,
чтобы при `MP_SERVER` вместо них вызывались наши. Для `DrawScreen` — `mp_imgui_draw_screen()`;
для `Init`, `Shutdown`, `NewFrame`, `UploadColorPair`, `SetAllocedPairCount` — пустые
функции, потому что цвета сервер передаёт индексами и палитру не выделяет.

Форма правки (по одному месту на вызов):

```cpp
#if defined(MP_SERVER)
    mp_imgui_draw_screen();
#else
    ImTui_ImplNcurses_DrawScreen();
#endif
```

Держать эти вставки минимальными: это единственный существующий файл с содержательной
правкой, и каждая строка здесь — будущий конфликт при вливании апстрима.

- [ ] **Step 5: Прогнать тесты**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
mpserver-tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Ожидается: семь тест-кейсов сессии проходят.

- [ ] **Step 6: Убедиться, что обычные сборки не задеты**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir=/tmp/cata-mp-test/
```

- [ ] **Step 7: Коммит**

```bash
git add src/mp/mp_imgui_server.cpp src/mp/mp_imgui_server.h src/cata_imgui.cpp \
        tests/mp_server_session_test.cpp
git commit -m "feat(mp): render imgui into the server screen through imtui"
```

---

### Task 9: Сокеты и TCP-транспорт

**Files:**
- Create: `src/mp/mp_socket.h`, `src/mp/mp_socket.cpp`, `src/mp/mp_tcp_transport.h`, `src/mp/mp_tcp_transport.cpp`
- Test: `tests/mp_tcp_transport_test.cpp`

**Interfaces:**
- Consumes: `mp_frame_encode`, `mp_frame_reader`, `mp_transport`.
- Produces:
  - `class mp_listener` — `static std::optional<mp_listener> bind( int port, std::string &err )`,
    `std::shared_ptr<mp_transport> accept( int timeout_ms )`
  - `std::shared_ptr<mp_transport> mp_tcp_connect( const std::string &host, int port, std::string &err )`

**Требования к реализации, каждое проверяется тестом ниже:**
- `TCP_NODELAY` на обоих концах.
- Неблокирующие сокеты; частичная запись копится в исходящем буфере и дописывается
  при следующей возможности, в том числе изнутри `recv`.
- Частичное чтение собирается через `mp_frame_reader`.
- `SIGPIPE` подавлен (`MSG_NOSIGNAL` на Linux, `SO_NOSIGPIPE` на macOS).
- `EINTR` не считается ошибкой.

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_tcp_transport_test.cpp`:

```cpp
#include "cata_catch.h"

#include <cstdint>
#include <string>
#include <vector>

#include "mp/mp_tcp_transport.h"
#include "mp/mp_transport.h"

// Port 0 asks the OS for a free port, so the suite never collides with a
// developer's running server or with a parallel test run.
TEST_CASE( "tcp transport carries messages over a real socket", "[mp]" )
{
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    REQUIRE( err.empty() );

    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    REQUIRE( err.empty() );

    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    client->send( { 7, 7, 7 } );
    const std::optional<std::vector<uint8_t>> got = server_side->recv( 1000 );
    REQUIRE( got.has_value() );
    CHECK( *got == std::vector<uint8_t> { 7, 7, 7 } );

    server_side->send( { 1 } );
    CHECK( client->recv( 1000 ) == std::vector<uint8_t> { 1 } );
}

TEST_CASE( "tcp transport reassembles a large message", "[mp]" )
{
    // Two hundred kilobytes is the order of a full redraw of a large terminal;
    // it will not fit in one socket write, so this exercises partial IO on
    // both ends.
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    std::vector<uint8_t> big( 200000 );
    for( size_t i = 0; i < big.size(); ++i ) {
        big[i] = static_cast<uint8_t>( i % 251 );
    }
    client->send( big );

    const std::optional<std::vector<uint8_t>> got = server_side->recv( 5000 );
    REQUIRE( got.has_value() );
    CHECK( *got == big );
}

TEST_CASE( "tcp transport reports a closed peer", "[mp]" )
{
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    client->close();
    // The peer may take a moment to notice; recv returning nothing with the
    // connection reported down is the contract.
    server_side->recv( 500 );
    CHECK_FALSE( server_side->connected() );
}

TEST_CASE( "tcp connect to a dead port fails with a message", "[mp]" )
{
    std::string err;
    // Port 1 is privileged and nothing will be listening on it.
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", 1, err );
    CHECK_FALSE( client );
    CHECK_FALSE( err.empty() );
}
```

- [ ] **Step 2: Прогнать и убедиться, что падает**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: `mp/mp_tcp_transport.h: No such file or directory`.

- [ ] **Step 3: Написать платформенный шим `mp_socket.h`/`.cpp`**

Содержимое: `using mp_socket_t = int` на POSIX и `SOCKET` на Windows; `mp_socket_startup()`
(на Windows зовёт `WSAStartup`, иначе ничего), `mp_socket_close`, `mp_socket_set_nonblocking`,
`mp_socket_set_nodelay`, `mp_socket_last_error_string()`, `mp_socket_poll( fd, want_read,
want_write, timeout_ms )`. Ни одна из этих функций не знает про игру.

- [ ] **Step 4: Написать `mp_tcp_transport`**

`send` кладёт кадр (`mp_frame_encode`) в исходящий буфер и пытается протолкнуть его сразу;
`recv` перед ожиданием дописывает остаток исходящего буфера, потом ждёт на
`mp_socket_poll` и наполняет `mp_frame_reader`. `connected()` становится ложным при
`recv() == 0`, при ошибке сокета или при `reader.failed()`.

- [ ] **Step 5: Прогнать тесты**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

Ожидается: четыре новых тест-кейса проходят.

- [ ] **Step 6: Убедиться, что `TCP_NODELAY` действительно выставлен**

```bash
grep -n "TCP_NODELAY" src/mp/mp_socket.cpp src/mp/mp_tcp_transport.cpp
```

Ожидается: выставляется и на принятом, и на подключённом сокете.

- [ ] **Step 7: Коммит**

```bash
git add src/mp/mp_socket.h src/mp/mp_socket.cpp src/mp/mp_tcp_transport.h \
        src/mp/mp_tcp_transport.cpp tests/mp_tcp_transport_test.cpp
git commit -m "feat(mp): add TCP transport over raw sockets"
```

---

### Task 10: Точка входа сервера

**Files:**
- Create: `src/mp/mp_server_main.h`, `src/mp/mp_server_main.cpp`
- Modify: `src/main.cpp`
- Test: ручная проверка (см. шаги) плюс тест хендшейка

**Interfaces:**
- Consumes: `mp_listener`, `mp_control`, `mp_server_set_terminal_size`, `mp_server_set_transport`.
- Produces:
  - `int mp_server_main( int port, const std::string &world_name )`
  - `bool mp_server_handshake( mp_transport &transport, std::string &reject_reason )`

- [ ] **Step 1: Написать падающий тест хендшейка**

Добавить в `tests/mp_server_session_test.cpp`:

```cpp
TEST_CASE( "handshake accepts a matching protocol version", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "serg";
    hello.hello.width = 100;
    hello.hello.height = 30;
    const std::string json = mp_control_to_json( hello );
    pair.b->send( mp_wrap( mp_msg_kind::control,
                           std::vector<uint8_t>( json.begin(), json.end() ) ) );

    std::string reason;
    CHECK( mp_server_handshake( *pair.a, reason ) );
    CHECK( reason.empty() );
    CHECK( mp_server_grid().width() == 100 );
    CHECK( mp_server_grid().height() == 30 );
}

TEST_CASE( "handshake refuses a protocol mismatch with an explanation", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION + 1;
    hello.hello.name = "serg";
    hello.hello.width = 100;
    hello.hello.height = 30;
    const std::string json = mp_control_to_json( hello );
    pair.b->send( mp_wrap( mp_msg_kind::control,
                           std::vector<uint8_t>( json.begin(), json.end() ) ) );

    std::string reason;
    CHECK_FALSE( mp_server_handshake( *pair.a, reason ) );
    CHECK_THAT( reason, Catch::Matchers::ContainsSubstring( "protocol" ) );
}

TEST_CASE( "handshake refuses a terminal that is too small", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "serg";
    hello.hello.width = 20;
    hello.hello.height = 5;
    const std::string json = mp_control_to_json( hello );
    pair.b->send( mp_wrap( mp_msg_kind::control,
                           std::vector<uint8_t>( json.begin(), json.end() ) ) );

    std::string reason;
    CHECK_FALSE( mp_server_handshake( *pair.a, reason ) );
    CHECK_THAT( reason, Catch::Matchers::ContainsSubstring( "terminal" ) );
}
```

Минимальный размер терминала взять из существующей константы проекта:

```bash
grep -rn "EIGHT_BY_EIGHT\|MIN_TERM\|TERMINAL_WIDTH_MIN\|minimum.*terminal" src/*.h src/*.cpp | head
```

- [ ] **Step 2: Прогнать и убедиться, что падает**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
```

- [ ] **Step 3: Написать хендшейк и главный цикл сервера**

`mp_server_handshake` читает первое сообщение с таймаутом, проверяет версию и размер
терминала, отправляет `hello_ack` и настраивает буфер экрана.

`mp_server_main` делает: `mp_socket_startup()`, `mp_listener::bind( port )`,
`accept` с ожиданием, `mp_server_handshake`, `mp_server_set_transport`, затем передаёт
управление обычному запуску игры — тому же пути, что и одиночная игра после
`catacurses::init_interface()` в `src/main.cpp:779`.

- [ ] **Step 4: Добавить аргументы в `src/main.cpp`**

В `first_pass_arguments` (`src/main.cpp:301`) добавить два обработчика по образцу
соседних: `--server <port>` и `--connect <host:port>`, каждый пишет в поля структуры
разбора аргументов. В `main()` (`:636`) до `catacurses::init_interface()` добавить
развилку:

```cpp
#if defined(MP_SERVER)
    if( cli.server_port > 0 ) {
        return mp_server_main( cli.server_port, cli.world );
    }
#endif
#if defined(MULTIPLAYER)
    if( !cli.connect_to.empty() ) {
        return mp_client_main( cli.connect_to );
    }
#endif
```

Держать эту вставку в одном месте и не больше десяти строк.

- [ ] **Step 5: Прогнать тесты и собрать сервер**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
mpserver-tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
make -j$(nproc) CCACHE=1 TESTS=0 MP_SERVER=1 BUILD_PREFIX=mpserver-
```

- [ ] **Step 6: Коммит**

```bash
git add src/mp/mp_server_main.h src/mp/mp_server_main.cpp src/main.cpp \
        tests/mp_server_session_test.cpp
git commit -m "feat(mp): add the server entry point and handshake"
```

---

### Task 11: Клиент `--connect`

**Files:**
- Create: `src/mp/mp_client_main.h`, `src/mp/mp_client_main.cpp`
- Test: ручная проверка (см. шаги)

**Interfaces:**
- Consumes: `mp_tcp_connect`, `mp_control`, `mp_screen_apply`, `mp_screen_grid`.
- Produces: `int mp_client_main( const std::string &host_and_port )`

**Что делает клиент.** Инициализирует свой обычный бэкенд curses, соединяется, шлёт `hello`
со своим размером терминала, затем крутит цикл: применяет присланные ячейки к своим окнам,
читает локальные нажатия и отправляет их немедленно, не дожидаясь ответа. Мира,
контента и правил у него нет.

- [ ] **Step 1: Написать скелет клиента**

Создать `src/mp/mp_client_main.h` и `.cpp`. Цикл:

```cpp
int mp_client_main( const std::string &host_and_port )
{
    std::string host;
    int port = 0;
    if( !mp_parse_host_port( host_and_port, host, port ) ) {
        std::cerr << "expected --connect host:port" << std::endl;
        return 1;
    }

    std::string err;
    std::shared_ptr<mp_transport> transport = mp_tcp_connect( host, port, err );
    if( !transport ) {
        std::cerr << "cannot connect to " << host_and_port << ": " << err << std::endl;
        return 1;
    }

    catacurses::init_interface();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = mp_client_player_name();
    hello.hello.width = get_terminal_width();
    hello.hello.height = get_terminal_height();
    hello.hello.colors_256 = catacurses::supports_256_colors();
    mp_send_control( *transport, hello );

    // ... await hello_ack, print the rejection reason and exit on refusal ...

    mp_screen_grid screen( hello.hello.width, hello.hello.height );
    while( transport->connected() ) {
        // Outgoing first: a keystroke must never wait for a redraw.
        mp_client_send_pending_input( *transport );
        const std::optional<std::vector<uint8_t>> msg = transport->recv( 16 );
        if( msg ) {
            mp_client_apply_message( *msg, screen );
        }
    }
    return 0;
}
```

- [ ] **Step 2: Отрисовка присланного экрана**

`mp_client_apply_message` для `mp_msg_kind::screen` вызывает `mp_screen_apply`, затем
выкладывает изменившиеся строки в `catacurses::stdscr` обычными вызовами
(`catacurses::wattron`, `catacurses::mvwprintw`) и делает `catacurses::wrefresh( stdscr )`.
Цвета: индексы 0–15 отдавать через существующую таблицу `all_colors`; индексы выше —
через 256-цветную ветку, если `supports_256_colors()`, иначе схлопывать в ближайший из 16.

Точное имя функции получения `nc_color` по индексу пары найти так:

```bash
grep -n "from_color_pair_index\|get_all_colors\|nc_color::from" src/color.h src/color.cpp | head
```

- [ ] **Step 3: Отправка ввода**

`mp_client_send_pending_input` берёт события через `inp_mngr.get_input_event()` своего
локального бэкенда с нулевым таймаутом и отправляет каждое как `mp_control_kind::input`.
Локально ничего не интерпретировать: сочетание клавиш превращает в действие сервер.

- [ ] **Step 4: Обработка изменения размера терминала**

При изменении локального размера (существующий путь `handle_resize`) клиент шлёт
`mp_control_kind::resize` и пересоздаёт свой `mp_screen_grid` под новый размер.

- [ ] **Step 5: Собрать и проверить руками**

Терминал 1:

```bash
mpserver-cataclysm-server --server 4321 --world testworld --userdir /tmp/cata-mp-server/
```

Терминал 2:

```bash
./cataclysm-tiles --connect 127.0.0.1:4321
```

Ожидается: в терминале 2 появляется главное меню игры, стрелки и Enter работают, создание
персонажа проходит, игра запускается, персонаж ходит.

- [ ] **Step 6: Коммит**

```bash
git add src/mp/mp_client_main.h src/mp/mp_client_main.cpp
git commit -m "feat(mp): add the thin client connect mode"
```

---

### Task 12: Обратное давление и схлопывание

**Files:**
- Modify: `src/mp/mp_server_screen.cpp`, `src/mp/mp_tcp_transport.cpp`
- Test: `tests/mp_server_session_test.cpp` (дополнить)

**Interfaces:**
- Consumes: `mp_transport::pending_bytes()` — новый метод интерфейса, по умолчанию 0.
- Produces: поведение «очередь переполнена — послать полную перерисовку вместо истории».

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "a backlogged client gets one full redraw instead of a history", "[mp]" )
{
    // A transport that never drains simulates a client that stopped reading.
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 80, 24 );
    mp_server_set_transport( pair.a );

    for( int i = 0; i < 200; ++i ) {
        catacurses::window win = catacurses::newwin( 1, 10, point( 0, i % 24 ) );
        catacurses::mvwprintw( win, point( 0, 0 ), "%d", i );
        catacurses::wrefresh( win );
        mp_server_flush_screen();
    }

    int screen_messages = 0;
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        screen_messages += kind == mp_msg_kind::screen ? 1 : 0;
    }
    // Without coalescing this would be 200.
    CHECK( screen_messages < 50 );
}
```

- [ ] **Step 2: Реализовать**

Добавить `virtual size_t pending_bytes() const { return 0; }` в `mp_transport`,
переопределить в TCP-транспорте (размер исходящего буфера) и в loopback (сумма размеров
очереди). В `mp_server_flush_screen`: если `pending_bytes()` превышает порог
(`MP_SCREEN_BACKLOG_BYTES = 512 * 1024`), отбросить накопленные исходящие экранные
сообщения, вызвать `grid.mark_all_dirty()` и отправить одну полную перерисовку.

Для отбрасывания понадобится `virtual void drop_pending_screen_messages()` — либо, проще,
транспорт хранит только последнее экранное сообщение, если следующее пришло раньше отправки.
Выбрать первое: явный метод понятнее в чтении.

- [ ] **Step 3: Прогнать тесты**

```bash
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
mpserver-tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/
```

- [ ] **Step 4: Коммит**

```bash
git add src/mp/mp_server_screen.cpp src/mp/mp_transport.h src/mp/mp_tcp_transport.cpp \
        src/mp/mp_loopback_transport.cpp tests/mp_server_session_test.cpp
git commit -m "feat(mp): collapse the screen backlog for a slow client"
```

---

### Task 13: CI, документация и ворота

**Files:**
- Create: `doc/MULTIPLAYER.md`
- Modify: файл конфигурации CI (найти на шаге 1)

**Interfaces:**
- Consumes: всё вышеперечисленное.
- Produces: зелёная сборка `MP_SERVER=1` в CI и инструкция для человека.

- [ ] **Step 1: Найти конфигурацию CI**

```bash
ls .github/workflows/
grep -rn "TILES=0\|make -j" .github/workflows/*.yml | head
```

- [ ] **Step 2: Добавить задание сборки сервера**

Добавить в существующий workflow задание, собирающее `MP_SERVER=1 TESTS=1` и прогоняющее
`cata_test "[mp]"`. Это прямое требование §10 спека: мультиплеерный путь одиночная игра не
прогоняет, поэтому без сторожа в CI он сгниёт.

- [ ] **Step 3: Написать `doc/MULTIPLAYER.md`**

Содержание: как собрать сервер, как его запустить, как подключиться, какие ограничения
первой версии (раскладка клавиш сервера, нет звука, нет TILES-карты, один игрок),
как читать лог управляющих сообщений при отладке.

- [ ] **Step 4: Ворота — вечер настоящей игры**

Поднять сервер на одной машине, подключиться с другой (или через Tailscale) и играть
не менее часа обычную одиночную партию: создание персонажа, перемещение, бой, крафт,
инвентарь, чтение, сон.

Записать в `docs/superpowers/reports/2026-XX-XX-mp-phase1-gate.md`:
- субъективное ощущение задержки при перемещении и в меню;
- есть ли артефакты отрисовки, и на каких экранах;
- работает ли печать вперёд;
- измеренный трафик (`ss -i` или `nethogs`) в спокойной игре и в бою;
- вердикт: продолжать к Ф2 или пересматривать модель клиента.

- [ ] **Step 5: Коммит**

```bash
git add doc/MULTIPLAYER.md .github/workflows/
git commit -m "docs: document the multiplayer server and add a CI guard"
```

---

## Самопроверка плана

**Покрытие спека.** §5.1 (роли, серверный бэкенд, отдельная цель) — задачи 5–8, 10.
§5.2 (модули `src/mp/`) — все задачи; `mp_sched` и `mp_turn` намеренно отсутствуют, это Ф3
и Ф4. §6.1 (композит, сброс перед ожиданием ввода, бинарь с прогонами) — задачи 2, 6, 7.
§6.2 (сообщения) — задача 3. §6.3 (размер терминала) — задачи 10, 11. §6.4 (версионирование
и отказ) — задача 10. §9 «медленный клиент» — задача 12. §10 (CI-сторож) — задача 13.
§11 Ф0 и Ф1 с воротами — задача 13, шаг 4.

Сознательно вне этого плана и записано как долг: `status`-сообщение (нужно только с
появлением второго игрока, Ф4), персональные раскладки, звук, TILES.

**Отклонение от спека, зафиксированное явно.** Спек §5.3 говорит, что под флагом
`MULTIPLAYER` остаются планировщик, сеть и серверный бэкенд. План оставляет чистые модули
(`mp_frame`, `mp_cell`, `mp_screen_codec`, `mp_protocol`, транспорты) тоже под `MULTIPLAYER`,
но собирает тесты с этим флагом, чтобы они гонялись в CI. Серверный бэкенд дополнительно
закрыт `MP_SERVER`. Это ужесточение, а не ослабление изоляции.

**Согласованность имён.** `mp_screen_grid` используется одинаково в задачах 2, 6, 8, 11, 12.
`mp_transport::send/recv/connected/close` — в задачах 4, 6, 7, 9, 10, 11; `pending_bytes` и
`drop_pending_screen_messages` добавляются в задаче 12 и там же реализуются во всех
наследниках. `mp_control` и `mp_control_kind` — задачи 3, 7, 10, 11. `mp_server_grid()`,
`mp_server_flush_screen()`, `mp_server_blit_window()` — задачи 6, 7, 8, 12.

**Места, где план сознательно оставляет решение исполнителю**, потому что точный ответ
виден только в коде и проверяется тут же компилятором: точные имена в API работы с JSON
(задача 3, шаг 5), имя переменной цели сборки в Makefile (задача 5, шаг 4), имя функции
перекодировки UTF-32 в UTF-8 (задача 8, шаг 3), константа минимального размера терминала
(задача 10, шаг 1), способ получить `nc_color` по индексу пары (задача 11, шаг 2). В каждом
случае в плане стоит команда, которой ответ находится за секунды.
