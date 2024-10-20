# Team Autobot
## Project Name: CircuitBot

### Description:
Circuit Bot is an edge-based personal assistant for home automation. It integrates ASR (Automatic Speech Recognition) and a LLaMA model to interpret user voice commands. This system allows users to control IR  based appliances directly, eliminating the need for internet connectivity. The primary goal is to provide localized, low-latency control over home appliances with flexibility for users to code, customize, and manage appliances through both the assistant and an accompanying locally hosted web app.

### Team Members:
1. [Hariprasath](https://github.com/hari-110)
2. [HemanthSai7](https://github.com/HemanthSai7)
3. [JacobJebaraj001](https://github.com/jacobjebaraj001)
4. [spectakural](https://github.com/spectakural)

### Installation
```bash
pip install -r requirements.txt
```

### Usage
```bash
$ cd src
$ python detect_from_microphone.py --chunk_size 3000 --model_path models/dnn_circuit_bot_3.onnx
```


# Images
- Model JSON Response aka tool calling output 
![Project Setup](assets/setup.jpg)
![CircuitBot](assets/tool_Call.png)