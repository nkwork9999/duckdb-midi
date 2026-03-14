#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/exception.hpp"

#include "MidiFile.h"

#include <string>
#include <vector>
#include <cmath>

namespace duckdb {

// ============================================
// midi_write(path, notes, durations, velocities, bpm)
// ノートリストからMIDIファイルを生成
//
// 例: SELECT midi_write('out.mid',
//       [60, 62, 64, 65, 67, 69, 71, 72],
//       [0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 1.0],
//       [80, 80, 80, 80, 80, 80, 80, 100],
//       120.0);
// ============================================

static std::vector<int32_t> ExtractIntList(const Value &val) {
	std::vector<int32_t> result;
	if (val.IsNull() || val.type().id() != LogicalTypeId::LIST) {
		return result;
	}
	auto &children = ListValue::GetChildren(val);
	for (auto &child : children) {
		if (!child.IsNull()) {
			result.push_back(child.GetValue<int32_t>());
		}
	}
	return result;
}

static std::vector<double> ExtractDblList(const Value &val) {
	std::vector<double> result;
	if (val.IsNull() || val.type().id() != LogicalTypeId::LIST) {
		return result;
	}
	auto &children = ListValue::GetChildren(val);
	for (auto &child : children) {
		if (!child.IsNull()) {
			result.push_back(child.GetValue<double>());
		}
	}
	return result;
}

// midi_write(path, notes[], durations[], velocities[], bpm)
static void MidiWriteFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto path = args.data[0].GetValue(i).GetValue<string>();
		auto notes = ExtractIntList(args.data[1].GetValue(i));
		auto durations = ExtractDblList(args.data[2].GetValue(i));
		auto velocities = ExtractIntList(args.data[3].GetValue(i));
		auto bpm = args.data[4].GetValue(i).GetValue<double>();

		if (notes.empty()) {
			throw InvalidInputException("midi_write: notes list cannot be empty");
		}
		if (durations.size() != notes.size()) {
			throw InvalidInputException("midi_write: durations list must match notes list length (%d vs %d)",
			                            durations.size(), notes.size());
		}
		if (velocities.size() != notes.size()) {
			throw InvalidInputException("midi_write: velocities list must match notes list length (%d vs %d)",
			                            velocities.size(), notes.size());
		}
		if (bpm <= 0) {
			throw InvalidInputException("midi_write: BPM must be positive, got %f", bpm);
		}

		int tpqn = 480;
		smf::MidiFile midifile;
		midifile.setTPQ(tpqn);
		midifile.addTrack(1); // track 0 = tempo, track 1 = notes

		// Set tempo on track 0
		int uspqn = static_cast<int>(60000000.0 / bpm);
		midifile.addTempo(0, 0, bpm);

		// Add notes on track 1
		int current_tick = 0;
		for (idx_t n = 0; n < notes.size(); n++) {
			int note = notes[n];
			int vel = velocities[n];
			int dur_ticks = static_cast<int>(durations[n] * tpqn * (bpm / 60.0));

			if (note < 0 || note > 127) {
				throw InvalidInputException("midi_write: note %d out of range 0-127", note);
			}
			if (vel < 0 || vel > 127) {
				vel = std::min(127, std::max(0, vel));
			}

			midifile.addNoteOn(1, current_tick, 0, note, vel);
			midifile.addNoteOff(1, current_tick + dur_ticks, 0, note);
			current_tick += dur_ticks;
		}

		midifile.sortTracks();

		if (!midifile.write(path)) {
			throw IOException("midi_write: failed to write file: %s", path);
		}

		result.SetValue(i, Value("Written " + std::to_string(notes.size()) + " notes to " + path));
	}
}

// midi_gen(path, preset) — プリセットでテスト用MIDIを生成
// presets: 'scale', 'chord', 'arpeggio', 'drum', 'chromatic'
static void MidiGenFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto path = args.data[0].GetValue(i).GetValue<string>();
		auto preset = args.data[1].GetValue(i).GetValue<string>();

		int tpqn = 480;
		double bpm = 120.0;
		smf::MidiFile midifile;
		midifile.setTPQ(tpqn);
		midifile.addTrack(1);
		midifile.addTempo(0, 0, bpm);

		std::string description;

		if (preset == "scale") {
			// C major scale up and down
			int notes[] = {60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62, 60};
			int tick = 0;
			for (int n : notes) {
				midifile.addNoteOn(1, tick, 0, n, 80);
				midifile.addNoteOff(1, tick + tpqn, 0, n);
				tick += tpqn;
			}
			description = "C major scale (15 notes)";

		} else if (preset == "chord") {
			// C-Am-F-G progression, 4 beats each
			int chords[][3] = {{60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}};
			int tick = 0;
			for (auto &chord : chords) {
				for (int n : chord) {
					midifile.addNoteOn(1, tick, 0, n, 80);
					midifile.addNoteOff(1, tick + tpqn * 4, 0, n);
				}
				tick += tpqn * 4;
			}
			description = "C-Am-F-G chord progression";

		} else if (preset == "arpeggio") {
			// C major arpeggio patterns
			int pattern[] = {60, 64, 67, 72, 67, 64};
			int tick = 0;
			for (int rep = 0; rep < 4; rep++) {
				for (int n : pattern) {
					midifile.addNoteOn(1, tick, 0, n, 70 + rep * 10);
					midifile.addNoteOff(1, tick + tpqn / 2, 0, n);
					tick += tpqn / 2;
				}
			}
			description = "C major arpeggio (24 notes)";

		} else if (preset == "drum") {
			// Basic rock beat on channel 9
			// kick=36, snare=38, hihat=42
			int tick = 0;
			for (int bar = 0; bar < 4; bar++) {
				for (int beat = 0; beat < 4; beat++) {
					// Hi-hat on every 8th note
					midifile.addNoteOn(1, tick, 9, 42, 60);
					midifile.addNoteOff(1, tick + tpqn / 4, 9, 42);
					midifile.addNoteOn(1, tick + tpqn / 2, 9, 42, 50);
					midifile.addNoteOff(1, tick + tpqn * 3 / 4, 9, 42);

					// Kick on 1 and 3
					if (beat == 0 || beat == 2) {
						midifile.addNoteOn(1, tick, 9, 36, 100);
						midifile.addNoteOff(1, tick + tpqn / 2, 9, 36);
					}
					// Snare on 2 and 4
					if (beat == 1 || beat == 3) {
						midifile.addNoteOn(1, tick, 9, 38, 90);
						midifile.addNoteOff(1, tick + tpqn / 2, 9, 38);
					}
					tick += tpqn;
				}
			}
			description = "4-bar rock drum beat";

		} else if (preset == "chromatic") {
			// All 128 MIDI notes
			int tick = 0;
			for (int n = 0; n < 128; n++) {
				midifile.addNoteOn(1, tick, 0, n, 64);
				midifile.addNoteOff(1, tick + tpqn / 4, 0, n);
				tick += tpqn / 4;
			}
			description = "Chromatic scale (128 notes)";

		} else {
			throw InvalidInputException(
			    "midi_gen: unknown preset '%s'. Available: scale, chord, arpeggio, drum, chromatic", preset);
		}

		midifile.sortTracks();

		if (!midifile.write(path)) {
			throw IOException("midi_gen: failed to write file: %s", path);
		}

		result.SetValue(i, Value("Generated " + preset + ": " + description + " → " + path));
	}
}

void RegisterMidiWriteFunctions(ExtensionLoader &loader) {
	// midi_write(path, notes[], durations[], velocities[], bpm)
	loader.RegisterFunction(ScalarFunction(
	    "midi_write",
	    {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::DOUBLE),
	     LogicalType::LIST(LogicalType::INTEGER), LogicalType::DOUBLE},
	    LogicalType::VARCHAR, MidiWriteFunction));

	// midi_gen(path, preset)
	loader.RegisterFunction(
	    ScalarFunction("midi_gen", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, MidiGenFunction));
}

} // namespace duckdb
