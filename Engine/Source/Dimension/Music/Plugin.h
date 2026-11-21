#pragma once
#include "Dimension/Core/Base.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// VST3 plug-ins: finding them, running them (instruments and effects) and their editor windows. The SDK stays inside
// Vst3Host.cpp; this header is plain C++.
namespace Dimension::Music
{
    struct Song;

    // A plug-in class a module offers (a module can hold several, like a bundle of effects).
    struct PluginInfo
    {
        std::string Name, Vendor, Version;
        std::string ClassId;       // 32 hex digits
        std::string SubCategories; // "Instrument|Synth", "Fx|Delay"...
        fs::path Module;           // the .vst3 bundle (or file)
        bool Instrument = false;
    };

    struct PluginParameter
    {
        uint32_t Id = 0;
        std::string Title, Units;
        double Default = 0.0;  // normalized 0..1
        int Steps = 0;         // 0 = continuous
        bool Automatable = true, ReadOnly = false, Hidden = false, Bypass = false;
    };

    // What the song is doing, for plug-ins that follow the tempo or position.
    struct PluginTransport
    {
        double Tempo = 120.0;
        bool Playing = false;
        double Quarters = 0.0;      // song position in beats
        int64_t Samples = 0;        // song position in samples
        int BeatsPerBar = 4;
    };

    namespace Plugins
    {
        // Built with VST3 hosting on this platform.
        bool IsSupported();
        // The standard VST3 folders of this system (user and system wide).
        std::vector<fs::path> DefaultFolders();
        // The audio plug-in classes of the .vst3 modules in `folders` (searched recursively; modules are loaded to read
        // their factories). Modules that fail add a line to `errors`.
        std::vector<PluginInfo> Scan(const std::vector<fs::path>& folders, std::vector<std::string>* errors = nullptr);
        std::vector<PluginInfo> ScanModule(const fs::path& module, std::string* error = nullptr);
    }

    // A running plug-in: its processor and controller. Main-thread calls (parameters, state, editor) and the audio
    // thread's (Process and the Queue calls) may overlap; the instance keeps them apart.
    class PluginInstance
    {
    public:
        static std::shared_ptr<PluginInstance> Create(const fs::path& module, const std::string& classId, double sampleRate, int maxBlock,
                                                      std::string& error);
        virtual ~PluginInstance() = default;

        virtual const std::string& GetName() const = 0;
        virtual bool IsInstrument() const = 0;
        virtual double GetSampleRate() const = 0;

        // Its whole state (the processor's and the controller's), as opaque bytes.
        virtual bool GetState(std::vector<uint8_t>& component, std::vector<uint8_t>& controller) = 0;
        virtual bool SetState(const std::vector<uint8_t>& component, const std::vector<uint8_t>& controller) = 0;

        // Parameters (main thread). Setting one tells both the controller (its editor follows) and the processor.
        virtual std::vector<PluginParameter> Parameters() = 0;
        virtual double GetParameter(uint32_t id) = 0;
        virtual void SetParameter(uint32_t id, double normalized) = 0;
        virtual std::string FormatParameter(uint32_t id, double normalized) = 0;

        // Audio thread: notes and parameter changes for the next Process, at a frame within it.
        virtual void QueueNote(int key, float velocity, bool on, int frame) = 0;
        virtual void QueueParameter(uint32_t id, double normalized, int frame) = 0;
        // Processes interleaved stereo in place (an effect's input; an instrument ignores what is there). At most the
        // block size it was made for.
        virtual void Process(float* stereo, uint32_t frames, const PluginTransport& transport) = 0;
        virtual void AllNotesOff() = 0;

        // Its own editor in a window of its own (main thread). False when it has none or the platform cannot host it.
        virtual bool HasEditor() = 0;
        virtual bool OpenEditor(const std::string& title) = 0;
        virtual void CloseEditor() = 0;
        virtual bool IsEditorOpen() const = 0;
        // Whether its editor changed something since the last call (the song should be saved).
        bool TakeEdited() { return m_Edited.exchange(false); }

    protected:
        std::atomic<bool> m_Edited{ false };
    };

    // The plug-ins a song uses, by the instance id each reference carries: made from the song's saved states, kept
    // while the song keeps them (so their sound and editors survive edits), let go when it no longer does.
    class PluginRack
    {
    public:
        PluginRack(double sampleRate, int maxBlock) : m_SampleRate(sampleRate), m_MaxBlock(maxBlock) {}
        std::shared_ptr<PluginInstance> Get(uint64_t id) const;
        // Makes what is missing (from module, class and state) and drops what the song no longer has. Errors (a plug-in
        // that is not installed) are kept for the UI: LastErrors.
        void Sync(const Song& song);
        // Writes each instance's state into the song's references (before saving or rendering).
        void StoreStates(Song& song);
        bool TakeEdited();
        const std::vector<std::string>& LastErrors() const { return m_Errors; }
        void Clear();

        // (used by Sync)
        std::shared_ptr<PluginInstance> Ensure(uint64_t id, const fs::path& module, const std::string& classId, const std::string& state,
                                               const std::string& controllerState);
        void Keep(const std::vector<uint64_t>& ids);

    private:
        double m_SampleRate;
        int m_MaxBlock;
        mutable std::mutex m_Mutex;
        std::map<uint64_t, std::shared_ptr<PluginInstance>> m_Instances;
        std::map<uint64_t, std::string> m_Failed; // ids that could not be made (not retried each edit)
        std::vector<std::string> m_Errors;
    };

    std::string Base64Encode(const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> Base64Decode(const std::string& text);
    // A fresh id for a plug-in reference.
    uint64_t NewPluginId();
}
