# Wake-word models

Each model is a pair of files: the network (`.tflite`) and its manifest
(`.json`), which records the phrase and the settings the model was trained
and tuned with. `../CMakeLists.txt` selects one model by name and reads its
settings from the manifest.

| Model | Phrase | Source | Licence |
| --- | --- | --- | --- |
| `hey_claude_run2` | "Hey Claude" | Training run 2 (2026-10-02) of `firmware/tools/wakeword_training/hey_claude.ipynb`; results in the run history of `docs/WAKEWORD-TRAINING.md` | Non-commercial use: some of the background-sound data it was trained with, and the L2-ARCTIC recordings behind one of its voices, are licensed for non-commercial use only |
| `hey_jarvis` | "Hey Jarvis" | [esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models), `models/v2/`, by Kevin Ahrendt | Apache License 2.0, copy in [LICENSE](LICENSE) |

The firmware builds `hey_claude_run2`, the best run so far, which does not
yet meet the accuracy targets. Trained models keep their run in their
name, so that the boot log and `wake` command show which run is running.
Once a run is chosen as final, earlier runs are deleted from this
directory. `hey_jarvis` is the stock model used while the detection
pipeline was brought up; it is kept for comparison until then. The
LICENSE file applies to `hey_jarvis` only.
