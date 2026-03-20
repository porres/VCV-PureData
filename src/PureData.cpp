
#include <rack.hpp>
// libpd
#include "z_libpd.h"
#include "util/z_print_util.h"
// vcv-puredata
#include <osdialog.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
//#include <efsw/efsw.h>
#if defined ARCH_WIN
    #include <windows.h>
#endif

using namespace rack;

static const int NUM_ROWS = 6;
static const int MAX_BUFFER_SIZE = 4096;

struct PureData;

struct ProcessBlock {
    float sampleRate = 0.f;
    float sampleTime = 0.f;
    int bufferSize = 1;
    float inputs[NUM_ROWS][MAX_BUFFER_SIZE] = {};
    float outputs[NUM_ROWS][MAX_BUFFER_SIZE] = {};
    float knobs[NUM_ROWS] = {};
    bool switches[NUM_ROWS] = {};
    float lights[NUM_ROWS][3] = {};
    float switchLights[NUM_ROWS][3] = {};
};

struct ScriptEngine {
    // Virtual methods for subclasses
    virtual ~ScriptEngine() {}
    virtual std::string getEngineName() {return "";}
    /** Executes the script.
    Return nonzero if failure, and set error message with setMessage().
    Called only once per instance.
    */
    virtual int run(const std::string& path, const std::string& script) {return 0;}

    /** Calls the script's process() method.
    Return nonzero if failure, and set error message with setMessage().
    */
    virtual int process() {return 0;}

    // Communication with PureData module.
    // These cannot be called from your constructor, so initialize your engine in the run() method.
    void display(const std::string& message);
    void setFrameDivider(int frameDivider);
    void setBufferSize(int bufferSize);
    ProcessBlock* getProcessBlock();
    // private
    PureData* module = NULL;
};

static const int BUFFERSIZE = MAX_BUFFER_SIZE * NUM_ROWS;

// there is no multi-instance support for receiving messages from libpd
// for now, received values for the module gui will be stored in global variables

static float g_lights[NUM_ROWS][3] = {};
static float g_switchLights[NUM_ROWS][3] = {};
static std::string g_utility[2] = {};
static bool g_display_is_valid = false;

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (getline(ss, item, delim)) {
        result.push_back(item);
    }
    return result;
}

struct LibPDEngine : ScriptEngine {
    t_pdinstance* _lpd = NULL;
    int _pd_block_size = 64;
    int _sampleRate = 0;
    int _ticks = 0;
    bool _init = true;

    float _old_knobs[NUM_ROWS] = {};
    bool  _old_switches[NUM_ROWS] = {};
    float _output[BUFFERSIZE] = {};
    float _input[BUFFERSIZE] = {};//  = (float*)malloc(1024*2*sizeof(float));
    const static std::map<std::string, int> _light_map;
    const static std::map<std::string, int> _switchLight_map;
    const static std::map<std::string, int> _utility_map;
    ~LibPDEngine() {
        if (_lpd)
            libpd_free_instance(_lpd);
    }
    void sendInitialStates(const ProcessBlock* block);
    static void receiveLights(const char* s);
    bool knobChanged(const float* knobs, int idx);
    bool switchChanged(const bool* knobs, int idx);
    void sendKnob(const int idx, const float value);
    void sendSwitch(const int idx, const bool value);
    std::string getEngineName() override {
        return "Pure Data";
    }
    int run(const std::string& path, const std::string& script) override {
        ProcessBlock* block = getProcessBlock();
        _sampleRate = block->sampleRate;
        setBufferSize(_pd_block_size);
        setFrameDivider(1);
        libpd_init();
        _lpd = libpd_new_instance();

        libpd_set_printhook((t_libpd_printhook)libpd_print_concatenator);
        libpd_set_concatenated_printhook(receiveLights);
// we now allow multiple instances
/*        if (libpd_num_instances() > 2) {
            display("Multiple simultaneous libpd (Pure Data) instances not yet supported.");
            return -1;
        }*/
        //display(std::to_string(libpd_num_instances()));
        libpd_init_audio(NUM_ROWS, NUM_ROWS, _sampleRate);

        // compute audio    [; pd dsp 1(
        libpd_start_message(1); // one enstry in list
        libpd_add_float(1.0f);
        libpd_finish_message("pd", "dsp");

        std::string version = "pd " + std::to_string(PD_MAJOR_VERSION) + "." +
                              std::to_string(PD_MINOR_VERSION) + "." +
                              std::to_string(PD_BUGFIX_VERSION);
        display(version);
//        std::string name = string::filename(path);
//        std::string dir  = string::directory(path);
        std::string name = system::getFilename(path);
        std::string dir  = system::getDirectory(path);
        libpd_openfile(name.c_str(), dir.c_str());

        sendInitialStates(block);

        return 0;
    }
    int process() override {
        // block
        ProcessBlock* block = getProcessBlock();

        // get samples
        int rows = NUM_ROWS;
        for (int s = 0; s < _pd_block_size; s++) {
            for (int r = 0; r < rows; r++) {
                _input[s * rows + r] = block->inputs[r][s];
            }
        }

        libpd_set_instance(_lpd);
        // knobs
        for (int i = 0; i < NUM_ROWS; i++) {
            if (knobChanged(block->knobs, i)) {
                sendKnob(i, block->knobs[i]);
            }
        }
        // lights
        for (int i = 0; i < NUM_ROWS; i++) {
            block->lights[i][0] = g_lights[i][0];
            block->lights[i][1] = g_lights[i][1];
            block->lights[i][2] = g_lights[i][2];
        }
        // switch lights
        for (int i = 0; i < NUM_ROWS; i++) {
            block->switchLights[i][0] = g_switchLights[i][0];
            block->switchLights[i][1] = g_switchLights[i][1];
            block->switchLights[i][2] = g_switchLights[i][2];
        }
        // switches
        for (int i = 0; i < NUM_ROWS; i++) {
            if (switchChanged(block->switches, i)) {
                sendSwitch(i, block->switches[i]);
            }
        }
        // display
        if (g_display_is_valid) {
            display(g_utility[1]);
            g_display_is_valid = false;
        }
        // process samples in libpd
        _ticks = 1;
        libpd_process_float(_ticks, _input, _output);

        // return samples
        for (int s = 0; s < _pd_block_size; s++) {
            for (int r = 0; r < rows; r++) {
                block->outputs[r][s] = _output[s * rows + r]; // scale up again to +-5V signal
                // there is a correction multiplier, because libpd's output is too quiet(?)
            }
        }
        return 0;
    }
};

void LibPDEngine::receiveLights(const char* s) {
    std::string str = std::string(s);
    std::vector<std::string> atoms = split(str, ' ');

    if (atoms[0] == "toRack:") {
        // parse lights list
        bool light_is_valid = true;
        int light_idx = -1;
        try {
            light_idx = _light_map.at(atoms[1]);      // map::at throws an out-of-range
        }
        catch (const std::out_of_range& oor) {
            light_is_valid = false;
            //display("Warning:"+atoms[1]+" not found!");
        }
        //std::cout << v[1] << ", " << g_led_map[v[1]] << std::endl;
        if (light_is_valid && atoms.size() == 5) {
            g_lights[light_idx][0] = stof(atoms[2]); // red
            g_lights[light_idx][1] = stof(atoms[3]); // green
            g_lights[light_idx][2] = stof(atoms[4]); // blue
        }
        else {
            // error
        }
        // parse switch lights list
        bool switchLight_is_valid = true;
        int switchLight_idx = -1;
        try {
            switchLight_idx = _switchLight_map.at(atoms[1]);      // map::at throws an out-of-range
        }
        catch (const std::out_of_range& oor) {
            switchLight_is_valid = false;
            //display("Warning:"+atoms[1]+" not found!");
        }
        //std::cout << v[1] << ", " << g_led_map[v[1]] << std::endl;
        if (switchLight_is_valid && atoms.size() == 5) {
            g_switchLights[switchLight_idx][0] = stof(atoms[2]); // red
            g_switchLights[switchLight_idx][1] = stof(atoms[3]); // green
            g_switchLights[switchLight_idx][2] = stof(atoms[4]); // blue
        }
        else {
            // error
        }
        // parse switch lights list
        bool utility_is_valid = true;
        try {
            _utility_map.at(atoms[1]);      // map::at throws an out-of-range
        }
        catch (const std::out_of_range& oor) {
            utility_is_valid = false;
            //g_display_is_valid = true;
            //display("Warning:"+atoms[1]+" not found!");
        }
        //std::cout << v[1] << ", " << g_led_map[v[1]] << std::endl;
        if (utility_is_valid && atoms.size() >= 3) {
            g_utility[0] = atoms[1]; // display
            g_utility[1] = "";
            for (unsigned i = 0; i < atoms.size() - 2; i++) {
                g_utility[1] += " " + atoms[i + 2]; // concatenate message
            }
            g_display_is_valid = true;
        }
        else {
            // error
        }
    }
    else {
        bool utility_is_valid = true;
        int utility_idx = -1;
        try {
            utility_idx = _utility_map.at(atoms[0]);      // map::at throws an out-of-range
        }
        catch (const std::out_of_range& oor) {
            WARN("PureData libpd: %s", s);
            utility_is_valid = false;
            //display("Warning:"+atoms[1]+" not found!");
            // print out on command line
        }
        if (utility_is_valid) {
            switch (utility_idx) {
                case 1:
                    WARN("PureData libpd: %s", s);
                    break;

                default:
                    break;
            }
        }
    }
}

bool LibPDEngine::knobChanged(const float* knobs, int i) {
    bool knob_changed = false;
    if (_old_knobs[i] != knobs[i]) {
        knob_changed = true;
        _old_knobs[i] = knobs[i];
    }
    return knob_changed;
}

bool LibPDEngine::switchChanged(const bool* switches, int i) {
    bool switch_changed = false;
    if (_old_switches[i] != switches[i]) {
        switch_changed = true;
        _old_switches[i] = switches[i];
    }
    return switch_changed;
}

const std::map<std::string, int> LibPDEngine::_light_map{
    { "L1", 0 },
    { "L2", 1 },
    { "L3", 2 },
    { "L4", 3 },
    { "L5", 4 },
    { "L6", 5 }
};

const std::map<std::string, int> LibPDEngine::_switchLight_map{
    { "S1", 0 },
    { "S2", 1 },
    { "S3", 2 },
    { "S4", 3 },
    { "S5", 4 },
    { "S6", 5 }
};

const std::map<std::string, int> LibPDEngine::_utility_map{
    { "display", 0 },
    { "error:", 1 }
};

void LibPDEngine::sendKnob(const int idx, const float value) {
    std::string knob = "K" + std::to_string(idx + 1);
    libpd_start_message(1);
    libpd_add_float(value);
    libpd_finish_message("fromRack", knob.c_str());
}

void LibPDEngine::sendSwitch(const int idx, const bool value) {
    std::string sw = "S" + std::to_string(idx + 1);
    libpd_start_message(1);
    libpd_add_float(value);
    libpd_finish_message("fromRack", sw.c_str());
}

void LibPDEngine::sendInitialStates(const ProcessBlock* block) {
    // knobs
    for (int i = 0; i < NUM_ROWS; i++) {
        sendKnob(i, block->knobs[i]);
        sendSwitch(i, block->knobs[i]);
    }

    for (int i = 0; i < NUM_ROWS; i++) {
        g_lights[i][0] = 0;
        g_lights[i][1] = 0;
        g_lights[i][2] = 0;
        g_switchLights[i][0] = 0;
        g_switchLights[i][1] = 0;
        g_switchLights[i][2] = 0;
    }

    //g_utility[0] = "";
    //g_utility[1] = "";

    //g_display_is_valid = false;
}

// ------------------- plugin ------------------------------

Plugin* pluginInstance;

static std::string settingsPdEditorPath =
#if defined ARCH_LIN
	"\"/usr/bin/pd-gui\"";
#else
	"";
#endif

json_t* settingsToJson() {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "pdEditorPath", json_string(settingsPdEditorPath.c_str()));
	return rootJ;
}

void settingsFromJson(json_t* rootJ) {
	json_t* pdEditorPathJ = json_object_get(rootJ, "pdEditorPath");
	if (pdEditorPathJ)
		settingsPdEditorPath = json_string_value(pdEditorPathJ);
}

void settingsLoad() { // Load plugin settings
	std::string filename = asset::user("Pd-PureData.json");
	FILE* file = std::fopen(filename.c_str(), "r");
	if (!file) {
		return;
	}
	DEFER({
		std::fclose(file);
	});

	json_error_t error;
	json_t* rootJ = json_loadf(file, 0, &error);
	if (rootJ) {
		settingsFromJson(rootJ);
		json_decref(rootJ);
	}
}

void settingsSave() {
	json_t* rootJ = settingsToJson();

	std::string filename = asset::user("VCV-PureData.json");
	FILE* file = std::fopen(filename.c_str(), "w");
	if (file) {
		json_dumpf(rootJ, file, JSON_INDENT(2) | JSON_REAL_PRECISION(9));
		std::fclose(file);
	}

	json_decref(rootJ);
}

std::string getApplicationPathDialog() {
	char* pathC = NULL;
#if defined ARCH_LIN
	pathC = osdialog_file(OSDIALOG_OPEN, "/usr/bin/", NULL, NULL);
#elif defined ARCH_WIN
	osdialog_filters* filters = osdialog_filters_parse("Executable:exe");
	pathC = osdialog_file(OSDIALOG_OPEN, "C:/", NULL, filters);
	osdialog_filters_free(filters);
#elif defined ARCH_MAC
	osdialog_filters* filters = osdialog_filters_parse("Application:app");
	pathC = osdialog_file(OSDIALOG_OPEN, "/Applications/", NULL, filters);
	osdialog_filters_free(filters);
#endif
	if (!pathC)
		return "";

	std::string path = "\"";
	path += pathC;
	path += "\"";
	std::free(pathC);
	return path;
}

void setPdEditorDialog() {
	std::string path = getApplicationPathDialog();
	if (path == "")
		return;
	settingsPdEditorPath = path;
	settingsSave();
}

struct PureData : Module {
	enum ParamIds {
		ENUMS(KNOB_PARAMS, NUM_ROWS),
		ENUMS(SWITCH_PARAMS, NUM_ROWS),
		NUM_PARAMS
	};
	enum InputIds {
		ENUMS(IN_INPUTS, NUM_ROWS),
		NUM_INPUTS
	};
	enum OutputIds {
		ENUMS(OUT_OUTPUTS, NUM_ROWS),
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(LIGHT_LIGHTS, NUM_ROWS * 3),
		ENUMS(SWITCH_LIGHTS, NUM_ROWS * 3),
		NUM_LIGHTS
	};

	std::string message;
	std::string path;
	std::string script;
	std::string engineName;
	std::mutex scriptMutex;
	ScriptEngine* scriptEngine = NULL;
	int frame = 0;
	int frameDivider;
	// This is dynamically allocated to have some protection against script bugs.
	ProcessBlock* block;
	int bufferIndex = 0;

//	efsw_watcher efsw = NULL;

	/** Script that has not yet been approved to load */
	std::string unsecureScript;
	bool securityRequested = false;
	bool securityAccepted = false;

	PureData() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		for (int i = 0; i < NUM_ROWS; i++)
			configParam(KNOB_PARAMS + i, 0.f, 1.f, 0.5f, string::f("Knob %d", i + 1));
		for (int i = 0; i < NUM_ROWS; i++)
			configParam(SWITCH_PARAMS + i, 0.f, 1.f, 0.f, string::f("Switch %d", i + 1));
		// for (int i = 0; i < NUM_ROWS; i++)
		// 	configInput(IN_INPUTS + i, string::f("#%d", i + 1));
		// for (int i = 0; i < NUM_ROWS; i++)
		// 	configOutput(OUT_OUTPUTS + i, string::f("#%d", i + 1));

		block = new ProcessBlock;
		setPath("");
	}

	~PureData() {
		setPath("");
		delete block;
	}

	void onReset() override {
		setScript(script);
	}

	void process(const ProcessArgs& args) override {
		// Load security-sandboxed script if the security warning message is accepted.
		if (unsecureScript != "" && securityAccepted) {
			setScript(unsecureScript);
			unsecureScript = "";
		}

		// Frame divider for reducing sample rate
		if (++frame < frameDivider)
			return;
		frame = 0;

		// Clear outputs if no script is running
		if (!scriptEngine) {
			for (int i = 0; i < NUM_ROWS; i++)
				for (int c = 0; c < 3; c++)
					lights[LIGHT_LIGHTS + i * 3 + c].setBrightness(0.f);
			for (int i = 0; i < NUM_ROWS; i++)
				for (int c = 0; c < 3; c++)
					lights[SWITCH_LIGHTS + i * 3 + c].setBrightness(0.f);
			for (int i = 0; i < NUM_ROWS; i++)
				outputs[OUT_OUTPUTS + i].setVoltage(0.f);
			return;
		}

		// Inputs
		for (int i = 0; i < NUM_ROWS; i++)
			block->inputs[i][bufferIndex] = inputs[IN_INPUTS + i].getVoltage();

		// Process block
		if (++bufferIndex >= block->bufferSize) {
			std::lock_guard<std::mutex> lock(scriptMutex);
			bufferIndex = 0;

			// Block settings
			block->sampleRate = args.sampleRate;
			block->sampleTime = args.sampleTime;

			// Params
			for (int i = 0; i < NUM_ROWS; i++)
				block->knobs[i] = params[KNOB_PARAMS + i].getValue();
			for (int i = 0; i < NUM_ROWS; i++)
				block->switches[i] = params[SWITCH_PARAMS + i].getValue() > 0.f;
			float oldKnobs[NUM_ROWS];
			std::memcpy(oldKnobs, block->knobs, sizeof(oldKnobs));

			// Run ScriptEngine's process function
			{
				// Process buffer
				if (scriptEngine) {
					if (scriptEngine->process()) {
						WARN("Patch %s process() failed. Stopped script.", path.c_str());
						delete scriptEngine;
						scriptEngine = NULL;
						return;
					}
				}
			}

			// Params
			// Only set params if values were changed by the script. This avoids issues when the user is manipulating them from the UI thread.
			for (int i = 0; i < NUM_ROWS; i++) {
				if (block->knobs[i] != oldKnobs[i])
					params[KNOB_PARAMS + i].setValue(block->knobs[i]);
			}
			// Lights
			for (int i = 0; i < NUM_ROWS; i++)
				for (int c = 0; c < 3; c++)
					lights[LIGHT_LIGHTS + i * 3 + c].setBrightness(block->lights[i][c]);
			for (int i = 0; i < NUM_ROWS; i++)
				for (int c = 0; c < 3; c++)
					lights[SWITCH_LIGHTS + i * 3 + c].setBrightness(block->switchLights[i][c]);
		}

		// Outputs
		for (int i = 0; i < NUM_ROWS; i++)
			outputs[OUT_OUTPUTS + i].setVoltage(block->outputs[i][bufferIndex]);
	}

	void setPath(std::string path) {
		// Cleanup
/*		if (efsw) {
			efsw_release(efsw);
			efsw = NULL;
		}*/
		this->path = "";
		setScript("");

		if (path == "")
			return;

		this->path = path;
		loadPath();

		if (this->script == "")
			return;

// Watch file
        // Old V1 code that causes  error
//        std::string dir = string::directory(path);
        // Correct V2 code, verified by the official API documentation
        std::string dir = rack::system::getDirectory(path);
        
//		efsw = efsw_create(false);
//		efsw_addwatch(efsw, dir.c_str(), watchCallback, false, this);
//		efsw_watch(efsw);
	}

	void loadPath() {
		// Read file
		std::ifstream file;
		file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
		try {
			file.open(path);
			std::stringstream buffer;
			buffer << file.rdbuf();
			std::string script = buffer.str();
			setScript(script);
		}
		catch (const std::runtime_error& err) {
			// Fail silently
		}
	}
    
    void setScript(std::string script) {
        std::lock_guard<std::mutex> lock(scriptMutex);
        // Reset script state
        if (scriptEngine) {
            delete scriptEngine;
            scriptEngine = NULL;
        }
        this->script = "";
        this->engineName = "";
        this->message = "";
        // Reset process state
        frameDivider = 32;
        frame = 0;
        bufferIndex = 0;
        // Reset block
        *block = ProcessBlock();

        if (script == "")
            return;
        this->script = script;

        // Create script engine from path extension
        std::string extension = system::getExtension(system::getFilename(path));
        // Remove the leading dot if it exists
        if (!extension.empty() && extension[0] == '.') {
            extension = extension.substr(1);
        }
//        scriptEngine = createScriptEngine(extension);
        scriptEngine = new LibPDEngine();
        if (!scriptEngine) {
            message = string::f("No engine for .%s extension", extension.c_str());
            return;
        }
        scriptEngine->module = this;

        // Run script
        if (scriptEngine->run(path, script)) {
            // Error message should have been set by ScriptEngine
            delete scriptEngine;
            scriptEngine = NULL;
            return;
        }
        this->engineName = scriptEngine->getEngineName();
    }

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		json_object_set_new(rootJ, "path", json_string(path.c_str()));

		std::string script = this->script;
		// If we haven't accepted the security of this script, serialize the security-sandboxed script anyway.
		if (script == "")
			script = unsecureScript;
		json_object_set_new(rootJ, "patch", json_stringn(script.data(), script.size()));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* pathJ = json_object_get(rootJ, "path");
		if (pathJ) {
			std::string path = json_string_value(pathJ);
			setPath(path);
		}

		// Only get the script string if the script file wasn't found.
		if (this->path != "" && this->script == "") {
			WARN("Patch file %s not found, using script in patch", this->path.c_str());
			json_t* scriptJ = json_object_get(rootJ, "patch");
			if (scriptJ) {
				std::string script = std::string(json_string_value(scriptJ), json_string_length(scriptJ));
				if (script != "") {
					// Request security warning message
					securityAccepted = false;
					securityRequested = true;
					unsecureScript = script;
				}
			}
		}
	}

	bool doesPathExist() {
		if (path == "")
			return false;
		// Try to open file
		std::ifstream file(path);
		return file.good();
	}

	void loadScriptDialog() {
		std::string dir = asset::plugin(pluginInstance, "examples");
		char* pathC = osdialog_file(OSDIALOG_OPEN, dir.c_str(), NULL, NULL);
		if (!pathC) {
			return;
		}
		std::string path = pathC;
		std::free(pathC);

		setPath(path);
	}

	void reloadScript() {
		loadPath();
	}

	void saveScriptDialog() {
		if (script == "")
			return;
//		std::string ext = string::filenameExtension(string::filename(path));
        std::string ext = system::getExtension(system::getFilename(path));
		std::string dir = asset::plugin(pluginInstance, "examples");
		std::string filename = "Untitled." + ext;
		char* newPathC = osdialog_file(OSDIALOG_SAVE, dir.c_str(), filename.c_str(), NULL);
		if (!newPathC) {
			return;
		}
		std::string newPath = newPathC;
		std::free(newPathC);
		// Add extension if user didn't specify one
//		std::string newExt = string::filenameExtension(string::filename(newPath));
        std::string newExt = system::getExtension(system::getFilename(newPath));
		if (newExt == "")
			newPath += "." + ext;
		// Write and close file
		{
			std::ofstream f(newPath);
			f << script;
		}
		// Load path so that it reloads and is watched.
		setPath(newPath);
	}

	void editScript() {
		std::string editorPath = getEditorPath();
		if (editorPath.empty())
			return;
		if (path.empty())
			return;
		// Launch editor and detach
#if defined ARCH_LIN
		std::string command = editorPath + " \"" + path + "\" &";
		(void) std::system(command.c_str());
#elif defined ARCH_MAC
		std::string command = "open -a " + editorPath + " \"" + path + "\" &";
		(void) std::system(command.c_str());
#elif defined ARCH_WIN
		std::string command = editorPath + " \"" + path + "\"";
		int commandWLen = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
		if (commandWLen <= 0)
			return;
		std::wstring commandW(commandWLen, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &commandW[0], commandWLen);
		STARTUPINFOW startupInfo;
		std::memset(&startupInfo, 0, sizeof(startupInfo));
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo;
		// Use the non-const [] accessor for commandW. Since C++11, it is null-terminated.
		if (CreateProcessW(NULL, &commandW[0], NULL, NULL, false, 0, NULL, NULL, &startupInfo, &processInfo)) {
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
		}
#endif
	}

	void setClipboardMessage() {
		glfwSetClipboardString(APP->window->win, message.c_str());
	}

	void appendContextMenu(Menu* menu) {
/*		struct NewScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->newScriptDialog();
			}
		};
		NewScriptItem* newScriptItem = createMenuItem<NewScriptItem>("New patch");
		newScriptItem->module = this;
		menu->addChild(newScriptItem);*/

		struct LoadScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->loadScriptDialog();
			}
		};
		LoadScriptItem* loadScriptItem = createMenuItem<LoadScriptItem>("Load patch");
		loadScriptItem->module = this;
		menu->addChild(loadScriptItem);

		struct ReloadScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->reloadScript();
			}
		};
		ReloadScriptItem* reloadScriptItem = createMenuItem<ReloadScriptItem>("Reload patch");
		reloadScriptItem->module = this;
		menu->addChild(reloadScriptItem);

		struct SaveScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->saveScriptDialog();
			}
		};
		SaveScriptItem* saveScriptItem = createMenuItem<SaveScriptItem>("Save patch as");
		saveScriptItem->module = this;
		menu->addChild(saveScriptItem);

		struct EditScriptItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->editScript();
			}
		};
		EditScriptItem* editScriptItem = createMenuItem<EditScriptItem>("Edit patch");
		editScriptItem->module = this;

		editScriptItem->disabled = !doesPathExist() || (getEditorPath() == "");
		menu->addChild(editScriptItem);

		menu->addChild(new MenuSeparator);

		struct SetPdEditorItem : MenuItem {
			void onAction(const event::Action& e) override {
				setPdEditorDialog();
			}
		};
		SetPdEditorItem* setPdEditorItem = createMenuItem<SetPdEditorItem>("Set Pure Data application");
		menu->addChild(setPdEditorItem);
	}

	std::string getEditorPath() {
		if (path == "")
			return "";
		// HACK check if extension is .pd
//		if (string::filenameExtension(string::filename(path)) == "pd")
//        if (system::getExtension(system::getFilename(path)) == "pd")
			return settingsPdEditorPath;
	}
};


void ScriptEngine::display(const std::string& message) {
	module->message = message;
}
void ScriptEngine::setFrameDivider(int frameDivider) {
	module->frameDivider = std::max(frameDivider, 1);
}
void ScriptEngine::setBufferSize(int bufferSize) {
	module->block->bufferSize = clamp(bufferSize, 1, MAX_BUFFER_SIZE);
}
ProcessBlock* ScriptEngine::getProcessBlock() {
	return module->block;
}


struct FileChoice : LedDisplayChoice {
	PureData* module;

	void step() override {
		if (module && module->engineName != "")
			text = module->engineName;
		else
			text = "Patch";
		text += ": ";
        if (module && module->path != ""){
//			text += string::filename(module->path);
            text += system::getFilename(module->path);
        }
		else
			text += "(click to load)";
	}

	void onAction(const event::Action& e) override {
		Menu* menu = createMenu();
		module->appendContextMenu(menu);
	}
};


struct MessageChoice : LedDisplayChoice {
	PureData* module;

	void step() override {
		text = module ? module->message : "";
	}

/*	void draw(const DrawArgs& args) override {
		nvgScissor(args.vg, RECT_ARGS(args.clipBox));
		if (font->handle >= 0) {
			nvgFillColor(args.vg, color);
			nvgFontFaceId(args.vg, font->handle);
			nvgTextLetterSpacing(args.vg, 0.0);
			nvgTextLineHeight(args.vg, 1.08);

			nvgFontSize(args.vg, 12);
			nvgTextBox(args.vg, textOffset.x, textOffset.y, box.size.x - textOffset.x, text.c_str(), NULL);
		}
		nvgResetScissor(args.vg);
	}*/

    void draw(const DrawArgs& args) override {
        nvgScissor(args.vg, RECT_ARGS(args.clipBox));
        // Load font - in V2, fonts are loaded from the system
        std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (font) {
            nvgFillColor(args.vg, color);
            nvgFontFaceId(args.vg, font->handle);
            nvgTextLetterSpacing(args.vg, 0.0);
            nvgTextLineHeight(args.vg, 1.08);
            nvgFontSize(args.vg, 12);
            nvgTextBox(args.vg, textOffset.x, textOffset.y, box.size.x - textOffset.x, text.c_str(), NULL);
        }
        nvgResetScissor(args.vg);
    }
    
    
	void onAction(const event::Action& e) override {
		Menu* menu = createMenu();

		struct SetClipboardMessageItem : MenuItem {
			PureData* module;
			void onAction(const event::Action& e) override {
				module->setClipboardMessage();
			}
		};
		SetClipboardMessageItem* item = createMenuItem<SetClipboardMessageItem>("Copy");
		item->module = module;
		menu->addChild(item);
	}
};


struct PureDataDisplay : LedDisplay {
	PureDataDisplay() {
		box.size = mm2px(Vec(69.879, 27.335));
	}

	void setModule(PureData* module) {
		FileChoice* fileChoice = new FileChoice;
		fileChoice->box.size.x = box.size.x;
		fileChoice->module = module;
		addChild(fileChoice);

		LedDisplaySeparator* fileSeparator = new LedDisplaySeparator;
		fileSeparator->box.size.x = box.size.x;
		fileSeparator->box.pos = fileChoice->box.getBottomLeft();
		addChild(fileSeparator);

		MessageChoice* messageChoice = new MessageChoice;
		messageChoice->box.pos = fileChoice->box.getBottomLeft();
		messageChoice->box.size.x = box.size.x;
		messageChoice->box.size.y = box.size.y - messageChoice->box.pos.y;
		messageChoice->module = module;
		addChild(messageChoice);
	}
};


struct PureDataWidget : ModuleWidget {
	PureDataWidget(PureData* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/PureData.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(8.099, 64.401)), module, PureData::KNOB_PARAMS + 0));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(20.099, 64.401)), module, PureData::KNOB_PARAMS + 1));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(32.099, 64.401)), module, PureData::KNOB_PARAMS + 2));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(44.099, 64.401)), module, PureData::KNOB_PARAMS + 3));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(56.099, 64.401)), module, PureData::KNOB_PARAMS + 4));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(68.099, 64.401)), module, PureData::KNOB_PARAMS + 5));
		addParam(createParamCentered<PB61303>(mm2px(Vec(8.099, 80.151)), module, PureData::SWITCH_PARAMS + 0));
		addParam(createParamCentered<PB61303>(mm2px(Vec(20.099, 80.151)), module, PureData::SWITCH_PARAMS + 1));
		addParam(createParamCentered<PB61303>(mm2px(Vec(32.099, 80.151)), module, PureData::SWITCH_PARAMS + 2));
		addParam(createParamCentered<PB61303>(mm2px(Vec(44.099, 80.151)), module, PureData::SWITCH_PARAMS + 3));
		addParam(createParamCentered<PB61303>(mm2px(Vec(56.099, 80.151)), module, PureData::SWITCH_PARAMS + 4));
		addParam(createParamCentered<PB61303>(mm2px(Vec(68.099, 80.151)), module, PureData::SWITCH_PARAMS + 5));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.099, 96.025)), module, PureData::IN_INPUTS + 0));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.099, 96.025)), module, PureData::IN_INPUTS + 1));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.099, 96.025)), module, PureData::IN_INPUTS + 2));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.099, 96.025)), module, PureData::IN_INPUTS + 3));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(56.099, 96.025)), module, PureData::IN_INPUTS + 4));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(68.099, 96.025)), module, PureData::IN_INPUTS + 5));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8.099, 112.25)), module, PureData::OUT_OUTPUTS + 0));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.099, 112.25)), module, PureData::OUT_OUTPUTS + 1));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(32.099, 112.25)), module, PureData::OUT_OUTPUTS + 2));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.099, 112.25)), module, PureData::OUT_OUTPUTS + 3));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(56.099, 112.25)), module, PureData::OUT_OUTPUTS + 4));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(68.099, 112.25)), module, PureData::OUT_OUTPUTS + 5));

		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(8.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 0));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(20.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 1));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(32.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 2));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(44.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 3));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(56.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 4));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(68.099, 51.4)), module, PureData::LIGHT_LIGHTS + 3 * 5));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(8.099, 80.151)), module, PureData::SWITCH_LIGHTS + 0));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(20.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 1));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(32.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 2));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(44.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 3));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(56.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 4));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(68.099, 80.151)), module, PureData::SWITCH_LIGHTS + 3 * 5));

		PureDataDisplay* display = createWidget<PureDataDisplay>(mm2px(Vec(3.16, 14.837)));
		display->setModule(module);
		addChild(display);
	}

	void appendContextMenu(Menu* menu) override {
		PureData* module = dynamic_cast<PureData*>(this->module);

		menu->addChild(new MenuSeparator);
		module->appendContextMenu(menu);
	}

	void onPathDrop(const event::PathDrop& e) override {
		PureData* module = dynamic_cast<PureData*>(this->module);
		if (!module)
			return;
		if (e.paths.size() < 1)
			return;
		module->setPath(e.paths[0]);
	}

	void step() override {
		PureData* module = dynamic_cast<PureData*>(this->module);
		if (module && module->securityRequested) {
			if (osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK_CANCEL, "VCV PureData is requesting to run a script from a patch or module preset. Running PureData scripts from untrusted sources may compromise your computer and personal information. Proceed and run script?")) {
				module->securityAccepted = true;
			}
			module->securityRequested = false;
		}
		ModuleWidget::step();
	}
};

void init(Plugin* p) {
	pluginInstance = p;

	p->addModel(createModel<PureData, PureDataWidget>("PureData"));
	settingsLoad();
}
