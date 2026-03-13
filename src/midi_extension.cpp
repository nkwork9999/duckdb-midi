#define DUCKDB_EXTENSION_MAIN

#include "midi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

void MidiExtension::Load(ExtensionLoader &loader) {
	// Scalar functions (midi_note_name, midi_note_freq, etc.)
	RegisterMidiScalarFunctions(loader);

	// Table functions
	RegisterReadMidiFunction(loader);
	RegisterMidiNotesFunction(loader);
	RegisterMidiMetaFunction(loader);

	// Playback functions
	RegisterMidiPlaybackFunctions(loader);
}

std::string MidiExtension::Name() {
	return "midi";
}

std::string MidiExtension::Version() const {
#ifdef EXT_VERSION_MIDI
	return EXT_VERSION_MIDI;
#else
	return "0.1.0";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void midi_duckdb_cpp_init(duckdb::ExtensionLoader &loader) {
	duckdb::MidiExtension ext;
	ext.Load(loader);
}

DUCKDB_EXTENSION_API void midi_init(duckdb::DatabaseInstance &db) {
	// Legacy init — redirect to cpp init is handled by DuckDB
}

DUCKDB_EXTENSION_API const char *midi_version() {
	return duckdb::DuckDB::LibraryVersion();
}

}
