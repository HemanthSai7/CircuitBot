import json
import time
import serial
import warnings
from langchain_ollama import ChatOllama
from langchain_core.messages import BaseMessage
from langchain_core.tools import tool
from langgraph.graph import END, StateGraph
from langgraph.prebuilt import ToolExecutor, ToolInvocation

warnings.filterwarnings("ignore")



def inference(transcription: str):
  model = ChatOllama(model="llama3.2:3b-instruct-q8_0", temperature=0.07)
  print(f"Transcription: {transcription}")
  ser = serial.Serial('/dev/ttyACM0', 115200)

  ser.write(b'get_details')
  time.sleep(1.0)
  json_data = ser.read_all()
  json_data = json_data.decode('utf-8')
  json_data = json_data.replace('\n', '')
  json_data = json_data.replace('\r', '')
  json_data = eval(json_data)
  json.dump(json_data, open('device_data.json', 'w'))


  devices_details = "\n".join([
          f"- {device['deviceName']}" for device in json_data
      ])

  device_operation_details = "\n".join([
          f"- ({device['deviceAction']}): Turns on the {device['deviceName']}" for device in json_data
      ])

  # devices_details = "helloworld"

  messages = """
  You are an IOT device control sequence generator. You will receive natural language queries from users wanting to control various IOT appliances. Generate a sequence of tool calls that implements the requested control logic according to the following specifications:

  Available Tools:
  1. turn_on(device): Turns on the specified device
  2. turn_off(device): Turns off the specified device
  3. delay(seconds): Waits for specified number of seconds
  4. increase(device): Increases the device's intensity/speed/volume
  5. decrease(device): Decreases the device's intensity/speed/volume

  Available Devices:
  """+devices_details+"""

  Response Format:
  Respond with a JSON array of tool calls, where each tool call is an object containing:
  - "tool": The name of the tool to call (string)
  - "parameters": An object containing the arguments for the tool
    - For device operations: {"device": "<device_name>"}
    - For delay: {"seconds": <number>}

  Example 1:
  User Query: "Blink light 2 times with 10 seconds gap in between"
  Response:
  ```json
  {
    "sequence": [
      {
        "tool": "turn_on",
        "parameters": {
          "device": "light"
        }
      },
      {
        "tool": "delay",
        "parameters": {
          "seconds": 2
        }
      },
      {
        "tool": "turn_off",
        "parameters": {
          "device": "light"
        }
      },
      {
        "tool": "delay",
        "parameters": {
          "seconds": 5
        }
      },
      {
        "tool": "turn_on",
        "parameters": {
          "device": "light"
        }
      },
      {
        "tool": "delay",
        "parameters": {
          "seconds": 2
        }
      },
      {
        "tool": "turn_off",
        "parameters": {
          "device": "light"
        }
      }
    ]
  }
  ```

  Example 2:
  User Query: "Turn on fan at maximum speed"
  Response:
  ```json
  {
    "sequence": [
      {
        "tool": "turn_on",
        "parameters": {
          "device": "fan"
        }
      },
      {
        "tool": "increase",
        "parameters": {
          "device": "fan"
        }
      },
      {
        "tool": "increase",
        "parameters": {
          "device": "fan"
        }
      },
      {
        "tool": "increase",
        "parameters": {
          "device": "fan"
        }
      }
    ]
  }
  ```

  Critical Requirements:
  1. Output must be valid JSON
  2. Strictly do not generate code
  3. Device names must be lowercase strings
  4. Tool names must exactly match the available tools list
  5. Delay seconds must be positive integers
  6. Each sequence must be wrapped in a root object with a "sequence" key
  7. Each tool call must include both "tool" and "args" fields
  8. Tool sequences must precisely implement the requested behavior

  Based on the above examples, generate a valid JSON sequence of tool calls that precisely implements the requested behavior while following all specifications.

  User query: """ + transcription + '\n'

  response = model.invoke(messages)

  print(response.content)

  @tool 
  def turn_on(device: str):
      """Turn on the light"""
      json_packet = [item for item in json_data if (item["deviceName"].lower() == device and item["deviceAction"].lower() == "on")][0]
      json_packet = f"data{{address={hex(json_packet['deviceAddress'])[2:].upper()},command={hex(json_packet['deviceCommand'])[2:].upper()}}}"
      print(json_packet)
      ser.write(json_packet.encode())
      return f"{device} turned on"

  @tool
  def turn_off(device: str):
      """Turn off the light"""
      json_packet = [item for item in json_data if (item["deviceName"].lower() == device and item["deviceAction"].lower() == "off")][0]
      json_packet = f"data{{address={hex(json_packet['deviceAddress'])[2:].upper()},command={hex(json_packet['deviceCommand'])[2:].upper()}}}"
      print(json_packet)
      ser.write(json_packet.encode())
      return f"{device} turned off"

  @tool
  def delay(seconds: int):
      """Wait for specified number of seconds"""
      ser.write(b'get_details')
      time.sleep(seconds)
      return f"Waited for {seconds} seconds"

  @tool
  def increase(device: str):
      """Increase the device's intensity/speed/volume"""
      json_packet = [item for item in json_data if (item["deviceName"].lower() == device and item["deviceAction"].lower() == "increase")][0]
      json_packet = f"data{{address={hex(json_packet['deviceAddress'])[2:].upper()},command={hex(json_packet['deviceCommand'])[2:].upper()}}}"
      print(json_packet)
      ser.write(json_packet.encode())
      return f"{device} increased"

  @tool
  def decrease(device: str):
      """Decrease the device's intensity/speed/volume"""
      json_packet = [item for item in json_data if (item["deviceName"].lower() == device and item["deviceAction"].lower() == "decrease")][0]
      json_packet = f"data{{address={hex(json_packet['deviceAddress'])[2:].upper()},command={hex(json_packet['deviceCommand'])[2:].upper()}}}"
      print(json_packet)
      ser.write(json_packet.encode())
      return f"{device} decreased"

  tools = [turn_on, turn_off, delay, increase, decrease]
  tool_executor = ToolExecutor(tools)

  tools_desc = "\n".join([
          f"- {tool.name}: {tool.description}" for tool in tools
      ])

  # print(tools[0].args_schema)

  cleaned_response = response.content.strip()
  if "```json" in cleaned_response:
      cleaned_response = cleaned_response.split("```json")[1].split("```")[0].strip()
  elif "```" in cleaned_response:
      cleaned_response = cleaned_response.split("```")[1].strip()

  print(cleaned_response)

  cleaned_response = eval(cleaned_response)

  results = []
  for step in cleaned_response["sequence"]:
      action = ToolInvocation(
          tool=step["tool"],
          tool_input=step.get("parameters", {})
      )
      result = tool_executor.invoke(action)
      results.append(result)
  # {"execution_results": results}