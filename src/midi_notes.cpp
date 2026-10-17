#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/exception.hpp"

#include "MidiFile.h"

#include <vector>
#include <string>
#include <array>

namespace duckdb {

// ============================================
// midi_notes(path) — NoteOn/Offをペアリングしてノート情報を返す
// ============================================

static const std::array<const char *, 12> NOTES_NOTE_NAMES = {"C", "C#", "D", "D#", "E", "F",
                                                               "F#", "G", "G#", "A", "A#", "B"};

struct NoteInfo {
	int32_t track_id;
	int32_t channel;
	int32_t note;
	std::string note_name;
	int32_t velocity;
	int64_t start_tick;
	int64_t end_tick;
	double start_sec;
	double end_sec;
	double duration_sec;
};

struct MidiNotesBindData : public TableFunctionData {
	std::string file_path;
	std::vector<NoteInfo> notes;

	MidiNotesBindData(const std::string &path) : file_path(path) {
		smf::MidiFile midifile;
		if (!midifile.read(path)) {
			throw IOException("Failed to read MIDI file: %s", path);
		}

		midifile.doTimeAnalysis();
		midifile.linkNotePairs();

		for (int track = 0; track < midifile.getTrackCount(); track++) {
			for (int i = 0; i < midifile[track].size(); i++) {
				auto &mev = midifile[track][i];

				if (!mev.isNoteOn()) {
					continue;
				}

				NoteInfo n;
				n.track_id = track;
				n.channel = mev.getChannel();
				n.note = mev.getKeyNumber();
				n.velocity = mev.getVelocity();
				n.start_tick = mev.tick;
				n.start_sec = mev.seconds;

				// Note name
				int pitch_class = n.note % 12;
				int octave = (n.note / 12) - 1;
				n.note_name = std::string(NOTES_NOTE_NAMES[pitch_class]) + std::to_string(octave);

				// Find linked note-off
				if (mev.isLinked()) {
					auto *off_ev = mev.getLinkedEvent();
					if (off_ev) {
						n.end_tick = off_ev->tick;
						n.end_sec = off_ev->seconds;
					} else {
						n.end_tick = n.start_tick;
						n.end_sec = n.start_sec;
					}
				} else {
					n.end_tick = n.start_tick;
					n.end_sec = n.start_sec;
				}
				n.duration_sec = mev.getDurationInSeconds();

				notes.push_back(std::move(n));
			}
		}
	}
};

struct MidiNotesState : public GlobalTableFunctionState {
	idx_t current_idx = 0;
};

static unique_ptr<FunctionData> MidiNotesBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {"track_id", "channel", "note", "note_name", "velocity",
	         "start_tick", "end_tick", "start_sec", "end_sec", "duration_sec"};
	return_types = {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::BIGINT,  LogicalType::BIGINT,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE, LogicalType::DOUBLE};

	auto path = input.inputs[0].GetValue<string>();
	return make_uniq<MidiNotesBindData>(path);
}

static unique_ptr<GlobalTableFunctionState> MidiNotesInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<MidiNotesState>();
}

static void MidiNotesScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->CastNoConst<MidiNotesBindData>();
	auto &state = data.global_state->Cast<MidiNotesState>();

	idx_t count = 0;
	idx_t batch_size = STANDARD_VECTOR_SIZE;

	while (state.current_idx < bind_data.notes.size() && count < batch_size) {
		auto &n = bind_data.notes[state.current_idx];

		output.SetValue(0, count, Value::INTEGER(n.track_id));
		output.SetValue(1, count, Value::INTEGER(n.channel));
		output.SetValue(2, count, Value::INTEGER(n.note));
		output.SetValue(3, count, Value(n.note_name));
		output.SetValue(4, count, Value::INTEGER(n.velocity));
		output.SetValue(5, count, Value::BIGINT(n.start_tick));
		output.SetValue(6, count, Value::BIGINT(n.end_tick));
		output.SetValue(7, count, Value::DOUBLE(n.start_sec));
		output.SetValue(8, count, Value::DOUBLE(n.end_sec));
		output.SetValue(9, count, Value::DOUBLE(n.duration_sec));

		state.current_idx++;
		count++;
	}

	output.SetCardinality(count);
}

void RegisterMidiNotesFunction(ExtensionLoader &loader) {
	TableFunction midi_notes("midi_notes", {LogicalType::VARCHAR}, MidiNotesScan, MidiNotesBind, MidiNotesInit);
	loader.RegisterFunction(midi_notes);
}

} // namespace duckdb
