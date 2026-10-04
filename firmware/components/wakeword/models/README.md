# Wake-word models

Each model is a pair of files: the network (`.tflite`) and its manifest
(`.json`), which records the phrase and the settings the model was trained
and tuned with. `../CMakeLists.txt` selects one model by name and reads its
settings from the manifest.

Beside them is the record of the training run that made the model, which
the build does not use: the training notebook's test report
(`_report.txt`) and its settings record (`_settings.json`), which holds
every setting of the run, the counts of samples and recordings, and the
versions of the packages it ran with.

| Model | Phrase | Source | Licence |
| --- | --- | --- | --- |
| `hey_claude_run3` | "Hey Claude" | Training run 3 (2026-10-04) of `firmware/tools/wakeword_training/hey_claude_v2.ipynb`, with recordings of a real voice; results in the run history of `docs/WAKEWORD-TRAINING.md` | Non-commercial use: some of the background-sound data it was trained with, and the L2-ARCTIC recordings behind one of its voices, are licensed for non-commercial use only |

The firmware builds `hey_claude_run3`, the model chosen from the training
runs so far. Its results, and what has not yet been measured, are in the
run history. Trained models keep their run in their name, so that the boot
log and `wake` command show which run is running, and the build stops if
a manifest names a file of another name.

Only the built model is kept here. The models of runs 1 and 2, and the
stock "Hey Jarvis" model used while the detection pipeline was brought up
(from [esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models),
`models/v2/`, by Kevin Ahrendt, Apache License 2.0), were removed on
2026-10-04. They remain in git history, and the stock model can be
fetched again from its source.
