#pragma once

#include "duckdb/main/extension.hpp"

namespace duckdb {

class MidiExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

// Registration functions for each module
void RegisterMidiScalarFunctions(ExtensionLoader &loader);
void RegisterReadMidiFunction(ExtensionLoader &loader);
void RegisterMidiNotesFunction(ExtensionLoader &loader);
void RegisterMidiMetaFunction(ExtensionLoader &loader);
void RegisterMidiPlaybackFunctions(ExtensionLoader &loader);

} // namespace duckdb
