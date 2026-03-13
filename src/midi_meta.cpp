#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/exception.hpp"

#include "MidiFile.h"

#include <vector>
#include <string>
#include <sstream>

namespace duckdb {

// ============================================
// midi_meta(path) — メタイベント（テンポ、拍子、調号等）を返す
// ============================================

struct MetaEvent {
	int32_t track_id;
	int64_t tick;
	double time_sec;
	std::string meta_type;
	std::string text_value;
	double num_value;
};

static std::string GetMetaTypeName(int meta_type) {
	switch (meta_type) {
	case 0x00: return "sequence_number";
	case 0x01: return "text";
	case 0x02: return "copyright";
	case 0x03: return "track_name";
	case 0x04: return "instrument_name";
	case 0x05: return "lyric";
	case 0x06: return "marker";
	case 0x07: return "cue_point";
	case 0x20: return "channel_prefix";
	case 0x2F: return "end_of_track";
	case 0x51: return "tempo";
	case 0x54: return "smpte_offset";
	case 0x58: return "time_signature";
	case 0x59: return "key_signature";
	case 0x7F: return "sequencer_specific";
	default: return "unknown_meta_" + std::to_string(meta_type);
	}
}

struct MidiMetaBindData : public TableFunctionData {
	std::string file_path;
	std::vector<MetaEvent> events;

	MidiMetaBindData(const std::string &path) : file_path(path) {
		smf::MidiFile midifile;
		if (!midifile.read(path)) {
			throw IOException("Failed to read MIDI file: %s", path);
		}

		midifile.doTimeAnalysis();

		for (int track = 0; track < midifile.getTrackCount(); track++) {
			for (int i = 0; i < midifile[track].size(); i++) {
				auto &mev = midifile[track][i];

				if (!mev.isMeta()) {
					continue;
				}

				MetaEvent e;
				e.track_id = track;
				e.tick = mev.tick;
				e.time_sec = mev.seconds;

				int meta_type = mev[1];
				e.meta_type = GetMetaTypeName(meta_type);
				e.num_value = 0;
				e.text_value = "";

				switch (meta_type) {
				case 0x51: {
					// Tempo: 3 bytes → microseconds per quarter note → BPM
					if (mev.size() >= 6) {
						int uspqn = (mev[3] << 16) | (mev[4] << 8) | mev[5];
						e.num_value = 60000000.0 / uspqn;
						e.text_value = std::to_string(uspqn) + " uspqn";
					}
					break;
				}
				case 0x58: {
					// Time signature: nn/2^dd
					if (mev.size() >= 7) {
						int numerator = mev[3];
						int denominator = 1 << mev[4];
						e.text_value = std::to_string(numerator) + "/" + std::to_string(denominator);
						e.num_value = numerator;
					}
					break;
				}
				case 0x59: {
					// Key signature: sf mi
					if (mev.size() >= 5) {
						int sf = (signed char)mev[3]; // sharps/flats
						int mi = mev[4];              // major/minor
						static const char *major_keys[] = {"Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F",
						                                   "C",  "G",  "D",  "A",  "E",  "B",  "F#", "C#"};
						static const char *minor_keys[] = {"Ab", "Eb", "Bb", "F",  "C",  "G",  "D",
						                                   "A",  "E",  "B",  "F#", "C#", "G#", "D#", "A#"};
						int idx = sf + 7;
						if (idx >= 0 && idx < 15) {
							e.text_value = std::string(mi == 0 ? major_keys[idx] : minor_keys[idx]) +
							               (mi == 0 ? " major" : " minor");
						}
						e.num_value = sf;
					}
					break;
				}
				case 0x01: case 0x02: case 0x03: case 0x04:
				case 0x05: case 0x06: case 0x07: {
					// Text-type meta events
					if (mev.size() > 3) {
						// Meta data starts after FF type length
						int len = mev[2];
						std::string text;
						for (int j = 3; j < 3 + len && j < mev.size(); j++) {
							text += static_cast<char>(mev[j]);
						}
						e.text_value = text;
					}
					break;
				}
				default:
					break;
				}

				events.push_back(std::move(e));
			}
		}
	}
};

struct MidiMetaState : public GlobalTableFunctionState {
	idx_t current_idx = 0;
};

static unique_ptr<FunctionData> MidiMetaBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	names = {"track_id", "tick", "time_sec", "meta_type", "text_value", "num_value"};
	return_types = {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::DOUBLE,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE};

	auto path = input.inputs[0].GetValue<string>();
	return make_uniq<MidiMetaBindData>(path);
}

static unique_ptr<GlobalTableFunctionState> MidiMetaInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<MidiMetaState>();
}

static void MidiMetaScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->CastNoConst<MidiMetaBindData>();
	auto &state = data.global_state->Cast<MidiMetaState>();

	idx_t count = 0;
	idx_t batch_size = STANDARD_VECTOR_SIZE;

	while (state.current_idx < bind_data.events.size() && count < batch_size) {
		auto &ev = bind_data.events[state.current_idx];

		output.SetValue(0, count, Value::INTEGER(ev.track_id));
		output.SetValue(1, count, Value::BIGINT(ev.tick));
		output.SetValue(2, count, Value::DOUBLE(ev.time_sec));
		output.SetValue(3, count, Value(ev.meta_type));
		output.SetValue(4, count, Value(ev.text_value));
		output.SetValue(5, count, Value::DOUBLE(ev.num_value));

		state.current_idx++;
		count++;
	}

	output.SetCardinality(count);
}

void RegisterMidiMetaFunction(ExtensionLoader &loader) {
	TableFunction midi_meta("midi_meta", {LogicalType::VARCHAR}, MidiMetaScan, MidiMetaBind, MidiMetaInit);
	loader.RegisterFunction(midi_meta);
}

} // namespace duckdb
