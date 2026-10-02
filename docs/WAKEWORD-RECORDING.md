# Recording real voices for wake-word training

How to record real people saying "Hey Claude", so that a training run can
learn from real voices as well as synthetic ones. Recordings are the most
effective way to improve detection for the people who actually use the
device, especially for accents that the synthetic voices do not cover (see
KNOWN-ISSUES R1). The training itself is described in
[WAKEWORD-TRAINING.md](WAKEWORD-TRAINING.md).

## Privacy first

Recordings of a real voice are personal data.

- They are uploaded only to the trainer's own Google Drive, where the
  training notebook reads them. They are never added to this repository,
  which ignores `recordings/` and `datasets/` directories as a safeguard.
- Record other people only with their agreement, and tell them where the
  recordings are kept and that they can be deleted.
- The trained model does not contain the recordings and cannot reproduce a
  voice; it can be shared like the synthetic-only models.

## What to record

### The wake phrase: about 100 recordings

Each recording is one "Hey Claude", said naturally. Variety matters more than
quantity: 100 varied recordings teach the model more than 300 identical ones.
A good spread:

| Vary | How | Share |
| --- | --- | --- |
| Distance from the microphone | 0.5 m, 1 m, 2 m, and across the room (3-4 m) | About a quarter each |
| Loudness | Quiet, normal, and raised as if calling from another room | Mostly normal |
| Speed | Quick, normal, and drawn out | Mostly normal |
| Tone | Plain, questioning ("Hey Claude?"), tired, cheerful, distracted | A mix |
| Direction | Facing the microphone, turned to the side, turned away | Mostly facing |
| Background | Quiet room, with a TV or music low, with a kitchen running | Mostly quiet |

Speak as you would to the device, not as you would to a recording. The
everyday version of the phrase, accent included, is what the model must
learn.

If several people will use the device, recordings from each of them help,
even 20-30 per person.

### Phrases that must not trigger: about 50 recordings (optional)

The same speakers saying things the device must ignore. These teach the model
that a familiar voice saying something close to the phrase is not the wake
word:

- "Claude" on its own, and in sentences: "I asked Claude about it",
  "Claude said no", "ask Claude later"
- Sound-alikes: "hey cloud", "hey Clyde", "hey clod", "okay Claude",
  "hi Claude", "hey Claudia"
- "Hey" followed by something else: "hey, come here", "hey, look at this"

### A test set: about 30 recordings, in a session of their own

Recordings that are never trained on, so that a run can be judged on real
voices rather than synthetic ones. The notebook's own test on synthetic
samples has proved a poor guide to how the device performs.

Record them as a separate session, on a different day or at least in a
separate take, mirroring the device test: 10 tries each at 0.5, 1 and 2 m,
and a few sound-alikes. A test set cut from the same takes as the training
recordings would contain near-copies of them and look better than it is.
Each distance goes in its own folder (see Uploading), so that the
notebook reports each separately. Once a run has been measured on it,
keep the test set as it is: runs are compared only on the same set, and
recordings added later make a new one.

## How to record

### Equipment

A phone's voice recorder app or the Windows Sound Recorder app is enough.
Place the phone or computer where the device will stand, so the distances in
the table above are measured from there.

### Format

- One recording per phrase is easiest to handle. Long takes are also fine:
  say the phrase, wait about 2 seconds in silence, say it again, and so on.
  A take of a few minutes holding 20-30 phrases saves a lot of tapping. The
  pauses let the training notebook cut the take into single phrases; the
  short pause inside "hey, Claude" stays within one phrase.
- WAV is best. MP3, M4A and the formats phone apps produce are converted by
  the notebook.
- Mono or stereo, any sample rate: everything is converted to 16 kHz mono,
  the rate of the device's microphone.
- Silence at the start and end of a recording does no harm: the notebook
  trims every phrase to its speech.

### Naming

Names help in checking the spread later, and the notebook's test on real
voices lists each test recording by its file name, but nothing else depends
on them. A pattern such as `alex_1m_normal_03.m4a`: who, distance, manner,
number. Use a first name or initials only.

## Uploading

Put the recordings in Google Drive, in the folder the training notebook
uses:

```
My Drive/hey_claude_wakeword/recordings/hey_claude/               the wake phrase
My Drive/hey_claude_wakeword/recordings/not_wake/                 phrases that must not trigger
My Drive/hey_claude_wakeword/recordings/test/hey_claude/0.5m/     the test set: the wake phrase at 0.5 m,
My Drive/hey_claude_wakeword/recordings/test/hey_claude/1m/         at 1 m,
My Drive/hey_claude_wakeword/recordings/test/hey_claude/2m/         at 2 m,
My Drive/hey_claude_wakeword/recordings/test/not_wake/alikes/       and phrases that must not trigger
```

Subfolders inside `hey_claude/` and `not_wake/` are fine, for example one
per person. The test set has the same two kinds; each folder inside them
is reported separately, so others, such as `test/hey_claude/3m/`, can be
added.

## How the recordings are used

Version 2 of the training notebook converts the recordings to 16 kHz
mono, cuts long takes at their pauses and trims each phrase to its speech,
printing how many phrases it found in each file. The wake-phrase and
sound-alike recordings join the synthetic samples in training, each used
as many differently augmented copies without artificial room echo. The
test set is never trained on: after training, the notebook runs the model
on it the way the device decides, and reports detections for each folder
and the score of each recording. The details are in
[WAKEWORD-TRAINING.md](WAKEWORD-TRAINING.md).

The device's own microphone would be the ideal recorder, since it is what the
model hears in use, but the firmware cannot yet save recordings. A phone or
computer at the device's position is a close substitute.
