# Wake-word training

How the "Hey Claude" model is trained, put into the firmware and tuned on
the device, with each run's results. The model is a microWakeWord network
trained in Google Colab on synthetic speech and recordings of real voices,
with
[firmware/tools/wakeword_training/hey_claude_v2.ipynb](../firmware/tools/wakeword_training/hey_claude_v2.ipynb).
How to make the recordings is in [WAKEWORD-RECORDING.md](WAKEWORD-RECORDING.md);
the accuracy targets are in KNOWN-ISSUES R1.

The device runs run 3's model. Its cutoff has not yet been tuned on the
device: that is the one item left before release (Tuning the cutoff on the
device, below).

Version 1 of the notebook, `hey_claude.ipynb` beside it, trained runs 1
and 2 and is kept as it was. Version 2 adds the recordings, a test on real
voices, and safe reruns. Sections of the notebook are named below by
their headings.

## Running it

1. Open the notebook in Google Colab: File → Upload notebook, and choose
   `hey_claude_v2.ipynb` from the repository.
2. Runtime → Change runtime type → T4 GPU (or a faster one where available).
3. Run **Install**. When it finishes, Runtime → Restart session.
4. In **Settings**, set `RUN` to the new run's name, such as `run3`. Run it,
   and then every section after it in order up to **Negative examples**.
   Helpers asks for access to Google Drive, where samples, recordings,
   checkpoints and results are kept between sessions.
5. In **Listen**, listen to each voice. If one mispronounces the phrase,
   change its spelling, or remove that voice from `SAMPLE_SETS`, in
   Settings, and rerun from Settings.
6. In **Recordings**, check that the number of phrases found in each take
   matches what was said, and listen to the examples it plays.
7. Restart the session (Runtime → Restart session), then run **Settings**,
   **Helpers**, **Train**, **Test on real voices** and **Keep results**.
   The earlier sections leave about 2 GB in use that training needs on the
   free plan.
   Training takes one to several hours on a free T4. It prints only the
   training process's memory use when that changes, so long silences are
   normal; at the end it prints the exit code (0 for success) and the end
   of the training log, `train.log`. An exit code of -9 means Colab stopped
   it for using too much memory.
8. If the session disconnects, start again from **Install**. Steps whose
   results are in Drive are skipped; those on the session's own disk, such
   as the features, are redone. Training starts from the weights of its
   last checkpoint, but runs all its steps again, so a disconnect late in
   training costs most of its time.

Before a new run is trained, the previous run's model can be scored on the
same test set, which gives the new run a comparison: set `RUN` to the
previous run, and run **Settings**, **Helpers**, **Recordings** and **Test
on real voices**. Its report is saved with that run in Drive.

**Keep results** downloads these files, each named after the run:

| File | What it is |
| --- | --- |
| `hey_claude_run3.tflite` | The model |
| `hey_claude_run3.json` | Its firmware manifest |
| `hey_claude_run3_report.txt` | The notebook's test: for each cutoff, the share of wake phrases missed (`frr`) and the false detections per hour (`faph`) |
| `hey_claude_run3_real_voice_test.txt` | The test on real voices, if there is a test set. It names the recordings, so it stays out of the repository |
| `hey_claude_run3_settings.json` | The run's settings, sample and recording counts, and package versions |

Git ignores these files in the repository's root, where a browser download
may land, but not anywhere else in the repository. Four of them belong in
it, beside the model (see the next section): the model, its manifest,
the report and the settings record, which together are the record of the
run. The settings record counts recordings by kind and names none. The
real-voice test names them, and stays out.

## Putting a model into the firmware

Each run's model keeps its run in its name, so that the device's boot log
and `wake` command show which run a test was made with.

1. Copy `hey_claude_run3.tflite`, `hey_claude_run3.json`,
   `hey_claude_run3_report.txt` and `hey_claude_run3_settings.json` to
   `firmware/components/wakeword/models/`, having read the last two
   against [SECRETS.md](../SECRETS.md). The build uses the first two.
   The manifest's cutoff is the lowest in the test report with under 0.5 false detections per hour, a
   starting point that on-device tuning adjusts. Its arena size is
   `ARENA` in Settings, measured for the model's shape; the firmware logs
   the bytes actually used at start-up, and the manifest is corrected if
   they differ.
2. Set `WAKEWORD_MODEL` to `hey_claude_run3` in
   `firmware/components/wakeword/CMakeLists.txt`, build and flash.
3. Describe the model in `firmware/components/wakeword/models/README.md`.
   Only the built model is kept in that directory: when a run replaces
   another as the firmware's model, the earlier run's files are deleted.
   They remain in git history and in Google Drive.

## Tuning the cutoff on the device

The multi-hour false-accept run measures false detections on the device
itself, the only measure of that target in KNOWN-ISSUES R1. It is made per
model, so again after any further training run.

1. Place the device, connected and in `idle`, beside continuous
   speech-heavy sound such as television or podcasts, with nobody saying
   the wake phrase.
2. On the console, `wake reset`.
3. Leave it for several hours.
4. `wake` gives the detections and the rate per hour: every one is false.
   The target is at most 0.5 per hour.
5. If it is over, raise the cutoff with `wake cutoff <value>`, which lasts
   until reboot, and repeat. Check that detection at 1 m still holds.
6. Write the chosen cutoff into the model's manifest in
   `firmware/components/wakeword/models/`, rebuild and flash, and record
   the result in the run history below.

## Run history

This table is the one record of each run's results; other documents refer
to it. Synthetic-test figures come from the notebook's own test on
augmented synthetic samples and the 5.3-hour test part of the dinner-party
recording, so they are harsher than a quiet room. Real-voice figures come
from Test on real voices, named by the test set's fingerprint; runs are
compared only on the same test set. Device figures were measured on the
breadboard with one speaker, saying the phrase 10 times at each distance,
unless the entry says otherwise.

| Run | Date | Notebook | Changes | Synthetic test at about 0.4 false detections per hour | Real voices | On the device |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 2026-10-01 | v1 | First run: 8,600 samples, background up to 5 dB louder than the voice, 10,000 steps | 53 % missed (cutoff 0.58); AUC 0.876 | Not measured | About half of tries detected at cutoff 0.60 |
| 2 | 2026-10-02 | v1 | 17,600 samples, background never louder than the voice, 30,000 steps | 16 % missed (cutoff 0.50, 0.375 per hour); AUC 0.298 | Test set `0eefa601a1481452`, at cutoff 0.50: 9, 8 and 8 of 10 at 0.5, 1 and 2 m; 3 of 5 sound-alikes triggered | At cutoff 0.50: 5 of 5 at 0.5 m (scores about 0.6), 5 of 10 at 1 m (hits about 0.51, misses about 0.48), 1 of 10 at 2 m (most scores under 0.2); "hey cloud" triggered once |
| 3 | 2026-10-04 | v2 | Run 2's samples, plus recordings of one real speaker: 100 of the wake phrase and 50 of phrases that must not trigger, each a fifth of the examples of its kind | 42 % missed (cutoff 0.83, 0.375 per hour); AUC 0.608 | Test set `0eefa601a1481452`, at cutoff 0.83: 10, 9 and 10 of 10 at 0.5, 1 and 2 m; 1 of 5 sound-alikes triggered ("hey cloud", score 0.949) | At cutoff 0.83, 7 tries at each distance by the recorded speaker: 7 of 7 at 0.5 m, 7 of 7 at 1 m, 6 of 7 at 2 m; "hey cloud" triggered once in the few times it was tried. A later counted test, 10 tries at each distance in a quiet room by the same speaker, worked well; its scores were not kept |

The AUC is the area under the curve of the share missed against false
detections per hour, so lower is better.

What the runs showed:

- **The notebook's test does not predict the device.** Run 2 missed 16 %
  in the test and about half at 1 m on the device, where its scores sat
  either side of the cutoff; at 2 m most were under 0.2. The stock "Hey
  Jarvis" model detected reliably at 2 m on the same hardware, so the
  shortfall was the model's. Synthetic training alone did not reach the
  targets, so run 3 added recordings of a real speaker.
- **Recordings work, for the voice recorded.** Run 3 is worse than run 2
  on synthetic voices (its cutoff rises to 0.83 to stay under 0.5 false
  detections per hour, missing 42 %), but better on the recorded
  speaker's test set, 29 of 30 against 25 of 30 with scores mostly above
  0.9, and on the device. It has specialised: both tests are by the
  speaker it was trained on, and the synthetic test suggests others will
  be missed more. Each further user should be recorded for a later run.
  The comparison is not clean, since run 3's recordings were added on top
  of the synthetic examples (KNOWN-ISSUES R16).
- **"Hey cloud" still gets through:** 0.949 in the real-voice test, above
  any usable cutoff. Only more recordings of it in a further run can fix
  that.
- **The real-voice test is a loose guide.** For run 2 it gave 8 of 10 at
  2 m where the device gave 1 of 10, probably because the recordings were
  not made with the device's microphone. It compares runs; the device
  test decides.
- **Run 3's cutoff is the notebook's starting value.** Its 0.375 false
  detections per hour are 2 in 5.3 hours of the dinner-party recording,
  at a cutoff chosen on that same recording, so the figure is optimistic
  until the device run above is made.

## What the notebook does, section by section

1. **Install** installs microWakeWord, the Piper sample generator and two
   audio libraries at pinned versions. The rest, including TensorFlow and
   the Hugging Face `datasets` library, is whatever Colab provides that
   day, so a later run may need fixes like those already recorded in the
   notebook. Each run records the full list of versions it used.
2. **Settings** holds everything a run might change, and nothing else: the
   run's name, the phrase's spellings, the voices and sample sets, the
   augmentation, how recordings are used, the training schedule and the
   model's shape. Values quoted below are those of run 3; each run's own
   are in its settings record. **Helpers** holds the functions the later
   sections share.
3. **Generate samples** makes about 17,600 spoken samples of "Hey Claude":
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
   of those voices is kept to a small share of the samples. Speaking speed
   varies from fast to slow.

   It also makes 5,000 samples of phrases that sound similar but must not
   trigger the device: "Claude" alone, "hey" alone, "hey cloud", "hey
   Clyde", "okay Claude" and others. "Claude" alone matters most, because
   people near the device will talk about Claude.
4. **Recordings** reads any real recordings from Google Drive, converts
   them to 16 kHz mono, cuts long takes into single phrases at their
   pauses, and trims every phrase to its speech. Trimming matters because
   microWakeWord trains on the end of each clip: silence left after a
   phrase would be learned as the phrase. It prints how many phrases it
   found in each file and plays a few, so the cutting can be checked by
   ear.
5. **Augmentation** and **Features** mix most samples with background sound,
   never louder than the voice, and add room echo to half, so the model
   hears the phrase as a microphone across a room would. Each recording is
   used as many differently augmented copies, without added echo, since
   recordings made across a room already carry a real one. All of it is
   converted into the same 40-band features the device computes.
6. **Negative examples** adds microWakeWord's ready-made negative examples:
   general speech, a dinner party, and sound without speech.
7. **Train** trains in two phases, the second at a lower learning rate.
   Recordings make up a set share of the wake-phrase examples and of the
   sound-alikes, a fifth in run 3. It keeps the checkpoint that detects
   the most synthetic test phrases while staying under 0.5 false detections
   per hour on about 1.5 hours of the dinner-party recording's validation
   part (about 9.7 hours in all). Only that share is used during training
   because the free Colab plan's 12.7 GB of memory cannot hold more; in
   practice it means no false detection at all on it. The final test uses
   the recording's separate test part, about 5.3 hours, so one false
   detection there reads as about 0.19 per hour. The result is converted to
   the quantised streaming model the device runs. A run keeps a
   fingerprint of the settings and features it started with, and Train
   refuses to continue it with others: a changed run needs a new name.
8. **Test on real voices** runs that model on the test recordings, which
   are never trained on, deciding as the device does: the same integer
   input to the model, and the same averaging of its scores against the
   cutoff. Each recording is placed between stretches of its own
   background sound, as the device hears a phrase within a continuous
   stream. It reports detections for each test folder at each cutoff, each
   recording's score, and a fingerprint of the test set, so that runs are
   compared only on the same set.
9. **Keep results** saves to Drive, and downloads: the model, the notebook's
   test report, the real-voice test, a firmware manifest, and a record of
   the run's settings and package versions.

Every step that makes something writes a marker holding a fingerprint of
the settings it was made with. A step is skipped only when its marker
matches, so a step stopped part-way, or one whose settings or inputs have
changed, is redone from the start. Samples made by version 1 carry no
marker; they are accepted if their settings are exactly those version 1
used, which Generate samples recognises by fingerprint, and made again
otherwise.

## Licences and privacy

Both versions of the notebook adapt microWakeWord's training notebook
(Apache License 2.0).
Some of the background-sound datasets it downloads, and the L2-ARCTIC
recordings behind one of its voices, are licensed for non-commercial use
only (CC BY-NC 4.0 for L2-ARCTIC), so a model trained with them is for
non-commercial use.

The synthetic samples contain no one's real voice. Recordings of real
people are personal data: they stay in the trainer's own Google Drive, are
converted only on the Colab session's own disk, and never enter this
repository, which also ignores `recordings/`, `training/` and `datasets/`
directories and common audio formats as a safeguard. The settings record
counts recordings without naming them; the real-voice test names them and
stays out of the repository.
