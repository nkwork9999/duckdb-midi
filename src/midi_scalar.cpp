#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <cmath>
#include <string>
#include <array>

namespace duckdb {

// Note names for pitch classes
static const std::array<const char *, 12> NOTE_NAMES = {"C", "C#", "D", "D#", "E", "F",
                                                         "F#", "G", "G#", "A", "A#", "B"};

// midi_note_name(60) → 'C4'
static void MidiNoteNameFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	UnaryExecutor::Execute<int32_t, string_t>(input, result, count, [&](int32_t note) {
		if (note < 0 || note > 127) {
			throw InvalidInputException("MIDI note number must be 0-127, got %d", note);
		}
		int pitch_class = note % 12;
		int octave = (note / 12) - 1;
		std::string name = std::string(NOTE_NAMES[pitch_class]) + std::to_string(octave);
		return StringVector::AddString(result, name);
	});
}

// midi_note_number('C4') → 60
static void MidiNoteNumberFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	UnaryExecutor::Execute<string_t, int32_t>(input, result, count, [&](string_t input_str) {
		std::string s = input_str.GetString();
		if (s.empty()) {
			throw InvalidInputException("Empty note name");
		}

		int pitch_class = -1;
		size_t pos = 1;

		char base = toupper(s[0]);
		switch (base) {
		case 'C': pitch_class = 0; break;
		case 'D': pitch_class = 2; break;
		case 'E': pitch_class = 4; break;
		case 'F': pitch_class = 5; break;
		case 'G': pitch_class = 7; break;
		case 'A': pitch_class = 9; break;
		case 'B': pitch_class = 11; break;
		default:
			throw InvalidInputException("Invalid note name: %s", s);
		}

		// Check for sharp/flat
		if (pos < s.size() && s[pos] == '#') {
			pitch_class++;
			pos++;
		} else if (pos < s.size() && s[pos] == 'b') {
			pitch_class--;
			pos++;
		}

		// Parse octave
		if (pos >= s.size()) {
			throw InvalidInputException("Missing octave in note name: %s", s);
		}

		int octave;
		try {
			octave = std::stoi(s.substr(pos));
		} catch (...) {
			throw InvalidInputException("Invalid octave in note name: %s", s);
		}

		int note = (octave + 1) * 12 + pitch_class;
		if (note < 0 || note > 127) {
			throw InvalidInputException("Resulting MIDI note %d out of range 0-127", note);
		}
		return note;
	});
}

// midi_note_freq(69) → 440.0
// A4 = 440Hz, equal temperament
static void MidiNoteFreqFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	UnaryExecutor::Execute<int32_t, double>(input, result, count, [&](int32_t note) {
		if (note < 0 || note > 127) {
			throw InvalidInputException("MIDI note number must be 0-127, got %d", note);
		}
		return 440.0 * std::pow(2.0, (note - 69) / 12.0);
	});
}

// midi_bpm_to_uspqn(120.0) → 500000
static void MidiBpmToUspqnFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	UnaryExecutor::Execute<double, int64_t>(input, result, count, [&](double bpm) {
		if (bpm <= 0) {
			throw InvalidInputException("BPM must be positive, got %f", bpm);
		}
		return static_cast<int64_t>(60000000.0 / bpm);
	});
}

// midi_uspqn_to_bpm(500000) → 120.0
static void MidiUspqnToBpmFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	UnaryExecutor::Execute<int64_t, double>(input, result, count, [&](int64_t uspqn) {
		if (uspqn <= 0) {
			throw InvalidInputException("Microseconds per quarter note must be positive, got %lld", uspqn);
		}
		return 60000000.0 / static_cast<double>(uspqn);
	});
}

void RegisterMidiScalarFunctions(ExtensionLoader &loader) {
	// midi_note_name(INTEGER) → VARCHAR
	loader.RegisterFunction(
	    ScalarFunction("midi_note_name", {LogicalType::INTEGER}, LogicalType::VARCHAR, MidiNoteNameFunction));

	// midi_note_number(VARCHAR) → INTEGER
	loader.RegisterFunction(
	    ScalarFunction("midi_note_number", {LogicalType::VARCHAR}, LogicalType::INTEGER, MidiNoteNumberFunction));

	// midi_note_freq(INTEGER) → DOUBLE
	loader.RegisterFunction(
	    ScalarFunction("midi_note_freq", {LogicalType::INTEGER}, LogicalType::DOUBLE, MidiNoteFreqFunction));

	// midi_bpm_to_uspqn(DOUBLE) → BIGINT
	loader.RegisterFunction(
	    ScalarFunction("midi_bpm_to_uspqn", {LogicalType::DOUBLE}, LogicalType::BIGINT, MidiBpmToUspqnFunction));

	// midi_uspqn_to_bpm(BIGINT) → DOUBLE
	loader.RegisterFunction(
	    ScalarFunction("midi_uspqn_to_bpm", {LogicalType::BIGINT}, LogicalType::DOUBLE, MidiUspqnToBpmFunction));
}

} // namespace duckdb
