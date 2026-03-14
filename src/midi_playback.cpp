#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/exception.hpp"

#include <string>
#include <atomic>
#include <thread>
#include <chrono>

#ifdef MIDI_PLAYBACK_MACOS
#include <AudioToolbox/AudioToolbox.h>
#include <mutex>

namespace {
// Global playback state
static std::mutex g_playback_mutex;
static MusicPlayer g_player = nullptr;
static MusicSequence g_sequence = nullptr;
static AUGraph g_graph = nullptr;
static std::atomic<bool> g_stop_requested{false};

void CleanupPlayback() {
	if (g_player) {
		MusicPlayerStop(g_player);
		DisposeMusicPlayer(g_player);
		g_player = nullptr;
	}
	if (g_sequence) {
		DisposeMusicSequence(g_sequence);
		g_sequence = nullptr;
	}
	if (g_graph) {
		AUGraphStop(g_graph);
		AUGraphClose(g_graph);
		DisposeAUGraph(g_graph);
		g_graph = nullptr;
	}
}

// Get total duration of the sequence in seconds
double GetSequenceDuration() {
	if (!g_sequence) return 0.0;

	UInt32 track_count = 0;
	MusicSequenceGetTrackCount(g_sequence, &track_count);

	MusicTimeStamp max_time = 0;
	for (UInt32 i = 0; i < track_count; i++) {
		MusicTrack track;
		MusicSequenceGetIndTrack(g_sequence, i, &track);
		MusicTimeStamp track_len = 0;
		UInt32 prop_size = sizeof(track_len);
		MusicTrackGetProperty(track, kSequenceTrackProperty_TrackLength, &track_len, &prop_size);
		if (track_len > max_time) {
			max_time = track_len;
		}
	}

	// Convert beats to seconds using tempo
	Float64 seconds = 0;
	MusicSequenceGetSecondsForBeats(g_sequence, max_time, &seconds);
	return seconds;
}

} // anonymous namespace
#endif

namespace duckdb {

// Setup AUGraph + MusicPlayer and start playing (helper)
#ifdef MIDI_PLAYBACK_MACOS
static void SetupAndPlay(const std::string &path) {
	OSStatus status;

	status = NewMusicSequence(&g_sequence);
	if (status != noErr) {
		throw IOException("Failed to create MusicSequence: %d", (int)status);
	}

	CFURLRef url = CFURLCreateFromFileSystemRepresentation(
	    kCFAllocatorDefault, (const UInt8 *)path.c_str(), path.size(), false);
	if (!url) {
		CleanupPlayback();
		throw IOException("Failed to create URL for: %s", path);
	}

	status = MusicSequenceFileLoad(g_sequence, url, kMusicSequenceFile_MIDIType, 0);
	CFRelease(url);
	if (status != noErr) {
		CleanupPlayback();
		throw IOException("Failed to load MIDI file: %s (error %d)", path, (int)status);
	}

	// Create AUGraph with DLS synth
	status = NewAUGraph(&g_graph);
	if (status != noErr) {
		CleanupPlayback();
		throw IOException("Failed to create AUGraph: %d", (int)status);
	}

	AudioComponentDescription synthDesc = {};
	synthDesc.componentType = kAudioUnitType_MusicDevice;
	synthDesc.componentSubType = kAudioUnitSubType_DLSSynth;
	synthDesc.componentManufacturer = kAudioUnitManufacturer_Apple;

	AudioComponentDescription outDesc = {};
	outDesc.componentType = kAudioUnitType_Output;
	outDesc.componentSubType = kAudioUnitSubType_DefaultOutput;
	outDesc.componentManufacturer = kAudioUnitManufacturer_Apple;

	AUNode synthNode, outNode;
	AUGraphAddNode(g_graph, &synthDesc, &synthNode);
	AUGraphAddNode(g_graph, &outDesc, &outNode);
	AUGraphConnectNodeInput(g_graph, synthNode, 0, outNode, 0);

	status = AUGraphOpen(g_graph);
	if (status != noErr) {
		CleanupPlayback();
		throw IOException("Failed to open AUGraph: %d", (int)status);
	}

	status = AUGraphInitialize(g_graph);
	if (status != noErr) {
		CleanupPlayback();
		throw IOException("Failed to initialize AUGraph: %d", (int)status);
	}

	MusicSequenceSetAUGraph(g_sequence, g_graph);

	status = NewMusicPlayer(&g_player);
	if (status != noErr) {
		CleanupPlayback();
		throw IOException("Failed to create MusicPlayer: %d", (int)status);
	}

	MusicPlayerSetSequence(g_player, g_sequence);
	MusicPlayerPreroll(g_player);

	status = AUGraphStart(g_graph);
	if (status != noErr) {
		CleanupPlayback();
		throw IOException("Failed to start AUGraph: %d", (int)status);
	}

	status = MusicPlayerStart(g_player);
	if (status != noErr) {
		CleanupPlayback();
		throw IOException("Failed to start playback: %d", (int)status);
	}
}
#endif

// midi_play('file.mid') — ブロッキング再生（再生完了まで待つ）
static void MidiPlayFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	UnaryExecutor::Execute<string_t, string_t>(input, result, count, [&](string_t path_str) {
		std::string path = path_str.GetString();

#ifdef MIDI_PLAYBACK_MACOS
		std::lock_guard<std::mutex> lock(g_playback_mutex);
		CleanupPlayback();
		g_stop_requested.store(false);

		SetupAndPlay(path);

		double duration = GetSequenceDuration();

		// Wait for playback to complete (poll every 100ms)
		auto start = std::chrono::steady_clock::now();
		while (!g_stop_requested.load()) {
			MusicTimeStamp current_time = 0;
			MusicPlayerGetTime(g_player, &current_time);

			Float64 current_sec = 0;
			MusicSequenceGetSecondsForBeats(g_sequence, current_time, &current_sec);

			if (current_sec >= duration) {
				break;
			}

			// Safety timeout: duration + 2 seconds
			auto elapsed = std::chrono::steady_clock::now() - start;
			if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > duration + 2) {
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		// Small extra delay for final notes to ring out
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		CleanupPlayback();

		char buf[128];
		snprintf(buf, sizeof(buf), "Played: %s (%.1f sec)", path.c_str(), duration);
		return StringVector::AddString(result, buf);
#else
		throw InvalidInputException("midi_play() is only supported on macOS (AudioToolbox)");
		return StringVector::AddString(result, "");
#endif
	});
}

// midi_stop() — 再生停止
static void MidiStopFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
#ifdef MIDI_PLAYBACK_MACOS
		g_stop_requested.store(true);
		std::lock_guard<std::mutex> lock(g_playback_mutex);
		CleanupPlayback();
		result.SetValue(i, Value("Playback stopped"));
#else
		result.SetValue(i, Value("midi_stop() is only supported on macOS"));
#endif
	}
}

void RegisterMidiPlaybackFunctions(ExtensionLoader &loader) {
	// midi_play(VARCHAR) → VARCHAR (blocking: waits for playback to finish)
	loader.RegisterFunction(
	    ScalarFunction("midi_play", {LogicalType::VARCHAR}, LogicalType::VARCHAR, MidiPlayFunction));

	// midi_stop() → VARCHAR
	loader.RegisterFunction(
	    ScalarFunction("midi_stop", {}, LogicalType::VARCHAR, MidiStopFunction));
}

} // namespace duckdb
