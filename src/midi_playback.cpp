#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/exception.hpp"

#include <string>
#include <atomic>

#ifdef MIDI_PLAYBACK_MACOS
#include <AudioToolbox/AudioToolbox.h>
#include <mutex>

namespace {
// Global playback state
static std::mutex g_playback_mutex;
static MusicPlayer g_player = nullptr;
static MusicSequence g_sequence = nullptr;
static AUGraph g_graph = nullptr;

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
} // anonymous namespace
#endif

namespace duckdb {

// midi_play('file.mid') — バックグラウンド再生
static void MidiPlayFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input = args.data[0];
	auto count = args.size();

	UnaryExecutor::Execute<string_t, string_t>(input, result, count, [&](string_t path_str) {
		std::string path = path_str.GetString();

#ifdef MIDI_PLAYBACK_MACOS
		std::lock_guard<std::mutex> lock(g_playback_mutex);

		// Stop any existing playback
		CleanupPlayback();

		// Load MIDI file into MusicSequence
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

		// Start playing
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

		return StringVector::AddString(result, "Playing: " + path);
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
		std::lock_guard<std::mutex> lock(g_playback_mutex);
		CleanupPlayback();
		result.SetValue(i, Value("Playback stopped"));
#else
		result.SetValue(i, Value("midi_stop() is only supported on macOS"));
#endif
	}
}

void RegisterMidiPlaybackFunctions(ExtensionLoader &loader) {
	// midi_play(VARCHAR) → VARCHAR
	loader.RegisterFunction(
	    ScalarFunction("midi_play", {LogicalType::VARCHAR}, LogicalType::VARCHAR, MidiPlayFunction));

	// midi_stop() → VARCHAR
	loader.RegisterFunction(
	    ScalarFunction("midi_stop", {}, LogicalType::VARCHAR, MidiStopFunction));
}

} // namespace duckdb
