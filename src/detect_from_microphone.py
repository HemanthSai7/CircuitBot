import sys
import time
import pyaudio
import argparse
import threading
import numpy as np
import sounddevice as sd
from openwakeword.model import Model
from llama_inference import inference
from transformers import WhisperProcessor, WhisperForConditionalGeneration

# Keyword list to check in ASR output (all lowercase for consistent comparison)
check = ["turn", "on", "off", "temperature", "volume", "increase", "decrease", "blink", "times", "speed", "tv", "TV"]

# Parse input arguments
parser = argparse.ArgumentParser()
parser.add_argument("--chunk_size", type=int, default=1280, required=False,
                    help="How much audio (in number of samples) to predict on at once")
parser.add_argument("--model_path", type=str, default="", required=False,
                    help="The path of a specific model to load")
parser.add_argument("--inference_framework", type=str, default='onnx', required=False,
                    help="The inference framework to use (either 'onnx' or 'tflite')")
args = parser.parse_args()

# Load openwakeword model
if args.model_path != "":
    owwModel = Model(wakeword_models=[args.model_path], inference_framework=args.inference_framework)
else:
    owwModel = Model(inference_framework=args.inference_framework)

n_models = len(owwModel.models.keys())

# Set up PyAudio stream
FORMAT = pyaudio.paInt16
CHANNELS = 1
RATE = 16000
CHUNK = args.chunk_size
audio = pyaudio.PyAudio()
mic_stream = audio.open(format=FORMAT, channels=CHANNELS, rate=RATE, input=True, frames_per_buffer=CHUNK)

# Load Whisper ASR model and processor
processor = WhisperProcessor.from_pretrained("openai/whisper-tiny.en")
model = WhisperForConditionalGeneration.from_pretrained("openai/whisper-tiny.en")

def record_audio(duration=5, sample_rate=16000):
    """Record audio from the microphone for a specified duration."""
    print(f"Recording for {duration} seconds...")
    sys.stdout.flush()  # Ensure print output is flushed
    audio_input = sd.rec(int(duration * sample_rate), samplerate=sample_rate, channels=1, dtype='float32')
    sd.wait()  # Wait until the recording is finished
    audio_input = audio_input.squeeze()  # Remove single-dimensional entries
    return audio_input, sample_rate

def process_asr():
    """Process audio input with Whisper model."""
    audio_input, sample_rate = record_audio(duration=5)

    if np.sum(np.abs(audio_input)) == 0:
        print("Timeout: No ASR input detected.")
        sys.stdout.flush()
        return None

    # Process audio with Whisper
    input_features = processor(audio_input, sampling_rate=sample_rate, return_tensors="pt").input_features
    predicted_ids = model.generate(input_features)
    transcription = processor.batch_decode(predicted_ids, skip_special_tokens=True)[0]
    print("ASR Transcription:", transcription)
    sys.stdout.flush()
    return transcription

def check_word_in_transcription(word, results_lock, results):
    """Check if a word from transcription is in the check list."""
    if word in check:
        with results_lock:  # Ensure only one thread writes to results at a time
            results.append(word)

def check_asr_output_multithread(transcription):
    """Check if ASR output contains at least two keywords from the check list using multiple threads."""
    transcription = transcription[:-1]
    words_in_transcription = transcription.lower().split()  # Ensure lowercase for matching
    results = []
    results_lock = threading.Lock()  # To safely modify the results list
    threads = []

    print(f"Words in transcription: {words_in_transcription}")  # Debugging info

    # Create a thread for each word in the transcription
    for word in words_in_transcription:
        thread = threading.Thread(target=check_word_in_transcription, args=(word, results_lock, results))
        threads.append(thread)
        thread.start()

    # Wait for all threads to finish
    for thread in threads:
        thread.join()

    # Debugging: show which words matched
    print(f"Matching words: {results}")
    
    # Now that all threads have finished, check the results
    if len(results) >= 2:
        print(f"ASR detected at least two keywords: {results}")
        print("transcription : ", transcription)
        sys.stdout.flush()
        return True
    # else:
    #     print("Timeout: Not enough keywords detected.")
    #     sys.stdout.flush()
    return False

def listen_for_wakeword():
    """Continuously listen for a wake word and trigger ASR on detection."""
    print("Listening for wakewords...")
    sys.stdout.flush()  # Ensure the print buffer is flushed

    while True:
        audio_chunk = np.frombuffer(mic_stream.read(CHUNK), dtype=np.int16)

        # Predict with openwakeword model
        prediction = owwModel.predict(audio_chunk)

        for mdl in owwModel.prediction_buffer.keys():
            scores = list(owwModel.prediction_buffer[mdl])
            curr_score = format(scores[-1], '.20f').replace("-", "")

            if scores[-1] > 0.3:
                print("\nWakeword detected! Switching to ASR mode...")
                sys.stdout.flush()
                return True

        sys.stdout.flush()
    return False

def wakeword_asr_pipeline():
    while True:
        if listen_for_wakeword():
            transcription = process_asr()

            # If transcription exists, check for keywords using multiple threads
            if transcription:
                asr_thread = threading.Thread(target=check_asr_output_multithread, args=(transcription,))
                asr_thread.start()
                asr_thread.join()  # Wait for keyword checking to finish

                if not check_asr_output_multithread(transcription):
                    # If timeout or insufficient keywords, return to wakeword detection
                    print("No valid ASR input. Returning to wakeword detection...")
                    sys.stdout.flush()
                    continue

                inference(transcription)
            else:
                # If timeout, return to wakeword detection
                print("Timeout during ASR. Returning to wakeword detection...")
                sys.stdout.flush()
                continue

            time.sleep(1)  # Add a small delay before returning to wakeword detection

if __name__ == "__main__":
    wakeword_asr_pipeline()