# Live chat for M5StickS3

Project by Steve Ruiz.

## Concept

This application uses a M5StickS3 as a user interface to chat with a large language model. The project is designed for Google's Gemini 3.1 Live API, which allows for low-latency conversational experience. The user communicates with the system by holding down the device's A button to record their voice; the recordings are sent via WiFi to a CloudFlare worker, which holds the conversation history for the device, passes the audio to the Gemini Live API, and then sends back the responses.

In addition to being able to respond in natural language, the model is also given various tool calls for accessing information or performing actions. It can access the internet via web fetch and web search, from a vector database of known information, as well as accessing information about the device, such as its settings and battery level. The model can perform actions on the device such as adjusting brightness, volume, or power, or displaying text and images, or playing sounds.
