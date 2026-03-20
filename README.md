# VCV PureData

[Pure Data](https://puredata.info) engine for [VCV Rack](https://vcvrack.com/) containing:
- 6 inputs
- 6 outputs
- 6 knobs
- 6 lights (RGB LEDs)
- 6 switches with RGB LEDs

This is maintained by Porres, please support me on <https://www.patreon.com/porres>.

This works is based on an old and abandoned project called Prototype, which used to run Pd patches via libpd. You don't need Pure Data installed in your system, but of course it's nice if you do as you can call it via VCV to edit patches.

## Build dependencies

Set up your build environment like described here, including the dependencies: https://vcvrack.com/manual/Building

Additionally:

### Windows
```bash
pacman -S mingw-w64-x86_64-premake
```

### Mac
```bash
brew install premake
```

### Ubuntu 16.04+
```bash
sudo apt install premake4
```

### Arch Linux
```bash
sudo pacman -S premake
```

## Build
### Add path to Rack-SDK
```bash
export RACK_DIR=/set/path/to/Rack-SDK/
```

### Make
```bash
make dep
make
```


## Contributors

- [Wes Milholen](https://grayscale.info/): panel design
- [Andrew Belt](https://github.com/AndrewBelt): host code
- [CHAIR](https://chair.audio) (Clemens Wegener, Max Neupert): libpd
- Porres, worked on the old prototype module, stripped down to Pd only, ported to VCV2 and made some other changes to create this project
