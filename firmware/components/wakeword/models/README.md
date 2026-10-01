# Wake-word models

Each model is a pair of files: the network (`.tflite`) and its manifest
(`.json`), which records the phrase and the settings the model was trained
and tuned with. `../CMakeLists.txt` selects one model by name and reads its
settings from the manifest.

| Model | Phrase | Source | Licence |
| --- | --- | --- | --- |
| `hey_jarvis` | "Hey Jarvis" | [esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models), `models/v2/`, by Kevin Ahrendt | Apache License 2.0, copy in [LICENSE](LICENSE) |

`hey_jarvis` is a stock model, used while the detection pipeline is brought
up. It is replaced by a model trained for this project's own phrase.
