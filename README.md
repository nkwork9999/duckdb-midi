# duckdb-midi

DuckDB extension for reading, analyzing, generating, and playing MIDI files with SQL.

```sql
LOAD midi;

-- Read MIDI events
SELECT * FROM read_midi('song.mid') WHERE event_type = 'note_on';

-- Analyze note frequency
SELECT note_name, count(*) as freq, round(avg(duration_sec), 2) as avg_dur
FROM midi_notes('song.mid')
GROUP BY note_name ORDER BY freq DESC;

-- Play
SELECT midi_play('song.mid');
```

## Build

```bash
# Requirements
brew install cmake ninja ccache

# Clone
git clone --recurse-submodules https://github.com/yourname/duckdb-midi.git
cd duckdb-midi

# Build
make release

# Run
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/midi/midi.duckdb_extension';
```

## API Reference

### Table Functions

#### `read_midi(path)`

Returns all MIDI events as a table.

| Column | Type | Description |
|--------|------|-------------|
| track_id | INTEGER | Track number |
| tick | BIGINT | Tick position |
| time_sec | DOUBLE | Time in seconds |
| event_type | VARCHAR | `note_on`, `note_off`, `control_change`, `program_change`, `pitch_bend`, `meta`, `other` |
| channel | INTEGER | MIDI channel (0-15, -1 for meta) |
| key | INTEGER | Note number / controller number |
| velocity | INTEGER | Velocity / controller value |
| raw_hex | VARCHAR | Raw bytes in hex |

```sql
SELECT track_id, tick, time_sec, event_type, channel, key, velocity
FROM read_midi('bach.mid')
WHERE event_type = 'note_on'
ORDER BY tick LIMIT 20;
```

#### `midi_notes(path)`

Returns note-on/off pairs with duration info.

| Column | Type | Description |
|--------|------|-------------|
| track_id | INTEGER | Track number |
| channel | INTEGER | MIDI channel |
| note | INTEGER | MIDI note number (0-127) |
| note_name | VARCHAR | Note name (e.g. `C4`, `F#5`) |
| velocity | INTEGER | Note-on velocity |
| start_tick | BIGINT | Start tick |
| end_tick | BIGINT | End tick |
| start_sec | DOUBLE | Start time in seconds |
| end_sec | DOUBLE | End time in seconds |
| duration_sec | DOUBLE | Duration in seconds |

```sql
SELECT note_name, count(*) as freq, round(avg(duration_sec), 2) as avg_dur
FROM midi_notes('bach.mid')
GROUP BY note_name ORDER BY freq DESC;
```

#### `midi_meta(path)`

Returns meta events (tempo, time signature, key signature, text, etc.).

| Column | Type | Description |
|--------|------|-------------|
| track_id | INTEGER | Track number |
| tick | BIGINT | Tick position |
| time_sec | DOUBLE | Time in seconds |
| meta_type | VARCHAR | `tempo`, `time_signature`, `key_signature`, `track_name`, `text`, `lyric`, etc. |
| text_value | VARCHAR | Human-readable value |
| num_value | DOUBLE | Numeric value (BPM for tempo, numerator for time sig, etc.) |

```sql
SELECT tick, round(num_value, 1) as bpm
FROM midi_meta('bach.mid')
WHERE meta_type = 'tempo'
ORDER BY tick;
```

### Scalar Functions

| Function | Input | Output | Example |
|----------|-------|--------|---------|
| `midi_note_name(n)` | INTEGER | VARCHAR | `midi_note_name(60)` → `'C4'` |
| `midi_note_number(s)` | VARCHAR | INTEGER | `midi_note_number('C4')` → `60` |
| `midi_note_freq(n)` | INTEGER | DOUBLE | `midi_note_freq(69)` → `440.0` |
| `midi_bpm_to_uspqn(bpm)` | DOUBLE | BIGINT | `midi_bpm_to_uspqn(120.0)` → `500000` |
| `midi_uspqn_to_bpm(uspqn)` | BIGINT | DOUBLE | `midi_uspqn_to_bpm(500000)` → `120.0` |

### Write / Generate Functions

#### `midi_write(path, notes, durations, velocities, bpm)`

Create a MIDI file from arrays.

```sql
SELECT midi_write('output.mid',
    [60, 64, 67, 72],           -- notes (MIDI numbers)
    [0.5, 0.5, 0.5, 1.0],      -- durations (seconds)
    [80, 70, 75, 100],          -- velocities (0-127)
    120.0                        -- BPM
);
```

#### `midi_gen(path, preset)`

Generate a preset MIDI file for testing.

| Preset | Description |
|--------|-------------|
| `scale` | C major scale up and down (15 notes) |
| `chord` | C-Am-F-G chord progression |
| `arpeggio` | C major arpeggio pattern (24 notes) |
| `drum` | 4-bar rock drum beat (ch.9) |
| `chromatic` | All 128 MIDI notes |

```sql
SELECT midi_gen('/tmp/test.mid', 'arpeggio');
```

### Playback Functions (macOS)

| Function | Description |
|----------|-------------|
| `midi_play(path)` | Play MIDI file (blocks until finished) |
| `midi_stop()` | Stop playback |

Uses macOS AudioToolbox with DLS Synth. Not available on Linux/Windows.

```sql
-- Generate and play
SELECT midi_gen('/tmp/demo.mid', 'chord');
SELECT midi_play('/tmp/demo.mid');
```

## Query Examples

### Track statistics
```sql
SELECT
    track_id,
    count(*) as note_count,
    midi_note_name(min(note)) as lowest,
    midi_note_name(max(note)) as highest,
    round(avg(velocity), 1) as avg_vel,
    round(sum(duration_sec), 2) as total_dur
FROM midi_notes('song.mid')
GROUP BY track_id ORDER BY track_id;
```

### Pitch class distribution
```sql
SELECT
    midi_note_name(note % 12) as pitch_class,
    count(*) as occurrences
FROM midi_notes('song.mid')
GROUP BY note % 12
ORDER BY occurrences DESC;
```

### Note density per second
```sql
SELECT
    floor(start_sec) as second,
    count(*) as notes_per_sec
FROM midi_notes('song.mid')
GROUP BY floor(start_sec)
ORDER BY second;
```

### Velocity dynamics (4-second windows)
```sql
SELECT
    floor(start_sec / 4) * 4 as time_window,
    round(avg(velocity), 0) as avg_vel,
    min(velocity) as min_vel,
    max(velocity) as max_vel
FROM midi_notes('song.mid')
GROUP BY floor(start_sec / 4)
ORDER BY time_window;
```

### Compare multiple files
```sql
SELECT 'original' as version, count(*) as notes, round(avg(velocity), 1) as avg_vel
FROM midi_notes('original.mid')
UNION ALL
SELECT 'remix', count(*), round(avg(velocity), 1)
FROM midi_notes('remix.mid');
```

## Architecture

```
DuckDB Engine
  └── duckdb-midi Extension
        ├── Table Functions (read_midi, midi_notes, midi_meta)
        │     └── craigsapp/midifile (SMF parser)
        ├── Scalar Functions (midi_note_name, midi_note_freq, ...)
        ├── Write Functions (midi_write, midi_gen)
        │     └── craigsapp/midifile (SMF writer)
        └── Playback (midi_play, midi_stop)
              └── macOS: AudioToolbox (DLS Synth)
```

## Platform Support

| Feature | macOS | Linux | Windows |
|---------|-------|-------|---------|
| read_midi / midi_notes / midi_meta | Yes | Yes | Yes |
| Scalar functions | Yes | Yes | Yes |
| midi_write / midi_gen | Yes | Yes | Yes |
| midi_play / midi_stop | Yes | - | - |

## Dependencies

- [DuckDB](https://duckdb.org/) v1.4.1
- [craigsapp/midifile](https://github.com/craigsapp/midifile) (BSD-2-Clause) — SMF parser/writer
- AudioToolbox (macOS built-in) — Playback backend

## License

MIT
