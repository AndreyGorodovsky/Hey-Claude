# Wake-word training

How the device's wake-word model for "Hey Claude" is trained, and how a new
model is put into the firmware. The model is a microWakeWord network, trained
on synthetic speech in Google Colab with
[firmware/tools/wakeword_training/hey_claude.ipynb](../firmware/tools/wakeword_training/hey_claude.ipynb).
Why synthetic speech, and how accuracy is judged, is in KNOWN-ISSUES R1.

## What the notebook does

1. Installs microWakeWord, the Piper sample generator and two audio
   libraries at pinned versions. The rest, including TensorFlow and the
   Hugging Face `datasets` library, is whatever Colab provides that day,
   so a later run may need fixes like those already recorded in the
   notebook.
2. Generates about 17,600 spoken samples of "Hey Claude":
   - 15,000 from a generator that blends 800 American English speakers
     (LibriTTS-R);
   - 1,000 from speakers of English as a second language (L2-ARCTIC:
     Arabic, Mandarin, Hindi, Korean, Spanish and Vietnamese first
     languages);
   - 1,000 from British and other English accents (VCTK);
   - 600 from Russian voices reading the phrase transliterated, which gives
     it a Russian accent. Piper's Hebrew voice would add another accent, but
     it needs a newer Piper than the sample generator supports.

   Some of the L2-ARCTIC and VCTK speakers mispronounce the phrase, so each
   of those voices is kept to a small share of the samples.

   Speaking speed varies from fast to slow.
3. Generates 5,000 samples of phrases that sound similar but must not
   trigger the device: "Claude" alone, "hey" alone, "hey cloud", "hey
   Clyde", "okay Claude" and others. "Claude" alone matters most, because
   people near the device will talk about Claude.
4. Mixes most samples with background sound, never louder than the voice,
   and adds room echo to half, so the model hears the phrase as a
   microphone across a room would.
5. Converts the audio into the same 40-band features the device computes,
   and adds microWakeWord's ready-made negative examples: general speech, a
   dinner party, and sound without speech.
6. Trains for 30,000 steps: 20,000, then 10,000 at a lower learning rate.
   It keeps the checkpoint that detects the most phrases while staying
   under 0.5 false detections per hour on about 1.5 hours of the
   dinner-party recording's validation part (about 9.7 hours in all). Only
   that share is used during training because the free Colab plan's 12.7 GB
   of memory cannot hold more; in practice it means no false detection at
   all on it. The final test uses the recording's separate test part, about
   5.3 hours, so one false detection there reads as about 0.19 per hour.
7. Converts the result to the quantised streaming model the device runs,
   tests it, and downloads it with its test report.

## Running it

1. Open the notebook in Google Colab: File → Upload notebook, and choose the
   `.ipynb` file from the repository.
2. Runtime → Change runtime type → T4 GPU (or a faster one where available).
3. Run cell 1. When it finishes, Runtime → Restart session.
4. Run the remaining cells in order. Cell 2 asks for access to Google Drive,
   where samples, checkpoints and results are kept between sessions.
5. In cell 4, listen to each voice. If one mispronounces the phrase, change
   its spelling in cell 2 or remove that voice in cell 5, and rerun cell 4.
6. Before training, restart the session (Runtime → Restart session) and run
   only cell 2 and then cell 11: the earlier cells leave about 2 GB in use
   that training needs on the free plan.
   Training (cell 11) takes one to several hours on a free T4. It prints
   only the training process's memory use when that changes, so long
   silences are normal; at the end it prints the exit code (0 for success)
   and the end of the training log, `train.log`. An exit code of -9 means
   Colab stopped it for using too much memory. If the session disconnects,
   run cells 1-10 again (generation steps already done are skipped), then
   restart the session and run cell 2 and cell 11, as above. Training
   starts from the weights of its last checkpoint, but runs the full 30,000
   steps again, so a disconnect late in training costs most of its time.
7. Cell 12 saves the model and its test report to Drive, named after the
   run (`hey_claude_run2.tflite` and `hey_claude_run2_report.txt`), and
   downloads both. The report gives, for each cutoff, the share of wake
   phrases missed (`frr`) and the false detections per hour (`faph`).

Each run is named by `RUN` in cell 2 and keeps its checkpoints in its own
folder in Drive, so a new run starts fresh. Samples already generated are
reused; the features are rebuilt in each new session.

## Putting a model into the firmware

Each run's model keeps its run in its name, so that the device's boot log
and `wake` command show which run a test was made with.

1. Copy the model to `firmware/components/wakeword/models/`, keeping its
   name, for example `hey_claude_run3.tflite`.
2. Write `hey_claude_run3.json` beside it, following the previous run's:
   - `wake_word`: "Hey Claude"
   - `model`: "hey_claude_run3.tflite"
   - `probability_cutoff`: from the test report, the lowest cutoff with
     under 0.5 false detections per hour. On-device tuning adjusts it.
   - `sliding_window_size`: 5, and `feature_step_size`: 10, as the
     notebook trains with
   - `tensor_arena_size`: the previous run's value, if the notebook's model
     layout has not changed; otherwise start with 30000. The firmware logs
     the bytes actually used at start-up; then set this to that figure,
     rounded up.
3. Set `WAKEWORD_MODEL` to `hey_claude_run3` in
   `firmware/components/wakeword/CMakeLists.txt`, build and flash.
4. Add the model to `firmware/components/wakeword/models/README.md`. Once a
   run is chosen as final, earlier runs' files are deleted; they remain in
   git history and in Google Drive.

## Run history

This table is the one record of each run's results; other documents refer
to it. Test figures come from the notebook's own test on augmented
synthetic samples and the 5.3-hour test part of the dinner-party recording,
so they are harsher than a quiet room. Device figures were measured on the
breadboard with one speaker, saying the phrase 10 times at each distance.

| Run | Date | Changes | Test at about 0.4 false detections per hour | On the device |
| --- | --- | --- | --- | --- |
| 1 | 2026-10-01 | First run: 8,600 samples, background up to 5 dB louder than the voice, 10,000 steps | 53 % missed (cutoff 0.58); AUC 0.876 | About half of tries detected at cutoff 0.60 |
| 2 | 2026-10-02 | 17,600 samples, background never louder than the voice, 30,000 steps | 16 % missed (cutoff 0.50, 0.375 per hour); AUC 0.298 | At cutoff 0.50: 5 of 5 at 0.5 m (scores about 0.6), 5 of 10 at 1 m (hits about 0.51, misses about 0.48), 1 of 10 at 2 m (most scores under 0.2); "hey cloud" triggered once |

The AUC is the area under the curve of the share missed against false
detections per hour, so lower is better.

Run 2 showed that the notebook's test does not predict the device: it
missed 16 % in the test and about half at 1 m on the device. At 1 m the
device's scores sat just either side of the cutoff, and at 2 m most were
under 0.2, so a lower cutoff cannot close the gap. The stock "Hey Jarvis"
model detected reliably at 2 m on the same hardware, which places the
shortfall in the model rather than the audio pipeline. Synthetic
training alone has not reached the targets in KNOWN-ISSUES R1, so run 3
adds recordings of real speakers, as described in
[WAKEWORD-RECORDING.md](WAKEWORD-RECORDING.md).

## Licences and privacy

The notebook adapts microWakeWord's training notebook (Apache License 2.0).
Some of the background-sound datasets it downloads, and the L2-ARCTIC
recordings behind one of its voices, are licensed for non-commercial use
only (CC BY-NC 4.0 for L2-ARCTIC), so a model trained with them is for
non-commercial use.

The samples are synthetic and contain no one's real voice. If recordings of
real people are added to a later run, they are personal data: they stay in
the trainer's own Google Drive and never enter this repository, which also
ignores the `training/` and `datasets/` directories as a safeguard.
