# lmodorg
Linux-native CLI skyrim mod manager using libfuse.

This project is an attempt to remedy the various bugs, performance problems, and general pains that come with modding bethesda games on Linux.
It works similarly to Mod Organizer 2, but unlike MO2, it has a full ("real") virtual file system read directly by the kernel, visible to all processes.

This means no more setting up of specific VFS executables or restarting multiple times to run DynDOLOD and Nemesis, just mount the VFS and do anything you want in it.

lmodorg also manages the entire game folder and not just ```/Data```, and allows installing mods like SKSE, ENBs and Engine Fixes in a managed way while keeping your real game clean.

## Usage
lmodorg reads the profile from ```profile.conf```, either in the current working directory or somewhere else with ```-C <PATH>```.
An example profile can be found in the ```example_profile``` directory.

```
$ lmodorg -h
usage: lmodorg [OPTIONS] COMMAND
options:
  -h, --help            Display this information.
  -v, --verbose         Print debugging information to stderr.
  -c, --color           Display output in multiple colors.
  -C, --profile=PATH    Use profile at PATH.
commands:
  lmodorg mount [OUTPUT]     Mount VFS with output directory OUTPUT, if no
                             OUTPUT is provided, OUTPUT is PROFILE/output.
  lmodorg new NAMES...       Create and enable empty mods NAMES.
  lmodorg remove NAMES...    Permanently delete mods NAMES.
  lmodorg enable NAMES...    Enable mods NAMES.
  lmodorg disable NAMES...   Disable mods NAMES.
  lmodorg install NAME PATH  Install archive at PATH to new mod NAME.
  lmodorg mods               List installed mods.
  lmodorg active             List active mods.
  lmodorg sort               Sort load order with LOOT.
  lmodorg autocreate         Generate autocreate lists without mounting a VFS.
```

To start lmodorg, run:
```
lmodorg mount
```
This first generates any configured `loadorder.txt`, `plugins.txt` or `archives.txt` files, then copies all config files found in `copy_files []`, then lastly builds the VFS from the active mods (`mods []`) and mounts it over the game directory (`game_root "/Path/To/Game"`).

Any edits made to the filesystem will be redirected to the output directory, which by default is located in `<PROFILE>/output`.
Be aware that this means that file deletions to the VFS will not be permanent unless the file is already overwritten by the output mod.

## Build

### Requirements
- libfuse 3.0 or higher
- GCC or clang
- GNU make
- unrar (optional)
- 7zip (optional)

Clone the repository, then run make. Use `DEBUG=1` to build with debug symbols, as well as ASan and UBSan.

```
git clone --recursive https://github.com/Lutf1sk/lmodorg/
sudo make install
```
