#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/exception.hpp"

#include "MidiFile.h"

#include <vector>
#include <string>

namespace duckdb {

// ============================================
// read_midi(path) — 全MIDIイベントをテーブルとして返す
// ============================================

struct MidiEvent {
	int32_t track_id;
	int64_t tick;
	double time_sec;
	std::string event_type;
	int32_t channel;
	int32_t key;
	int32_t velocity;
	std::string raw_hex;
};

struct ReadMidiBindData : public TableFunctionData {
	std::string file_path;
	std::vector<MidiEvent> events;

	ReadMidiBindData(const std::string &path) : file_path(path) {
		smf::MidiFile midifile;
		if (!midifile.read(path)) {
			throw IOException("Failed to read MIDI file: %s", path);
		}

		midifile.doTimeAnalysis();
		midifile.linkNotePairs();

		for (int track = 0; track < midifile.getTrackCount(); track++) {
			for (int event_idx = 0; event_idx < midifile[track].size(); event_idx++) {
				auto &mev = midifile[track][event_idx];
				MidiEvent e;
				e.track_id = track;
				e.tick = mev.tick;
				e.time_sec = mev.seconds;

				// Raw hex
				std::ostringstream hex_ss;
				for (int i = 0; i < mev.size(); i++) {
					if (i > 0) hex_ss << " ";
					hex_ss << std::hex << std::uppercase;
					hex_ss.width(2);
					hex_ss.fill('0');
					hex_ss << (int)(unsigned char)mev[i];
				}
				e.raw_hex = hex_ss.str();

				// Parse event type
				if (mev.isNoteOn()) {
					e.event_type = "note_on";
					e.channel = mev.getChannel();
					e.key = mev.getKeyNumber();
					e.velocity = mev.getVelocity();
				} else if (mev.isNoteOff()) {
					e.event_type = "note_off";
					e.channel = mev.getChannel();
					e.key = mev.getKeyNumber();
					e.velocity = mev.getVelocity();
				} else if (mev.isController()) {
					e.event_type = "control_change";
					e.channel = mev.getChannel();
					e.key = mev[1];     // controller number
					e.velocity = mev[2]; // value
				} else if (mev.isProgramChange()) {
					e.event_type = "program_change";
					e.channel = mev.getChannel();
					e.key = mev[1]; // program number
					e.velocity = 0;
				} else if (mev.isPitchbend()) {
					e.event_type = "pitch_bend";
					e.channel = mev.getChannel();
					e.key = mev.getP1() | (mev.getP2() << 7);
					e.velocity = 0;
				} else if (mev.isMeta()) {
					e.event_type = "meta";
					e.channel = -1;
					e.key = mev[1]; // meta type
					e.velocity = 0;
				} else {
					e.event_type = "other";
					e.channel = mev.size() > 0 ? (mev[0] & 0x0F) : -1;
					e.key = 0;
					e.velocity = 0;
				}

				events.push_back(std::move(e));
			}
		}
	}
};

struct ReadMidiState : public GlobalTableFunctionState {
	idx_t current_idx = 0;
};

static unique_ptr<FunctionData> ReadMidiBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	names = {"track_id", "tick", "time_sec", "event_type", "channel", "key", "velocity", "raw_hex"};
	return_types = {LogicalType::INTEGER, LogicalType::BIGINT,  LogicalType::DOUBLE,
	                LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::INTEGER, LogicalType::VARCHAR};

	auto path = input.inputs[0].GetValue<string>();
	return make_uniq<ReadMidiBindData>(path);
}

static unique_ptr<GlobalTableFunctionState> ReadMidiInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ReadMidiState>();
}

static void ReadMidiScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->CastNoConst<ReadMidiBindData>();
	auto &state = data.global_state->Cast<ReadMidiState>();

	idx_t count = 0;
	idx_t batch_size = STANDARD_VECTOR_SIZE;

	while (state.current_idx < bind_data.events.size() && count < batch_size) {
		auto &ev = bind_data.events[state.current_idx];

		output.SetValue(0, count, Value::INTEGER(ev.track_id));
		output.SetValue(1, count, Value::BIGINT(ev.tick));
		output.SetValue(2, count, Value::DOUBLE(ev.time_sec));
		output.SetValue(3, count, Value(ev.event_type));
		output.SetValue(4, count, Value::INTEGER(ev.channel));
		output.SetValue(5, count, Value::INTEGER(ev.key));
		output.SetValue(6, count, Value::INTEGER(ev.velocity));
		output.SetValue(7, count, Value(ev.raw_hex));

		state.current_idx++;
		count++;
	}

	output.SetCardinality(count);
}

void RegisterReadMidiFunction(ExtensionLoader &loader) {
	TableFunction read_midi("read_midi", {LogicalType::VARCHAR}, ReadMidiScan, ReadMidiBind, ReadMidiInit);
	loader.RegisterFunction(read_midi);
}

} // namespace duckdb
