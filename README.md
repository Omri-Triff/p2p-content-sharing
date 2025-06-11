
A C-based peer-to-peer file sharing system inspired by Napster architecture.  
Clients discover each other using UDP multicast and communicate over TCP.  
If no peer has the requested file, the system gracefully falls back to a central server.

---

## 🔧 Features

- 🔍 **Peer Discovery** using multicast (UDP)
- 🔄 **TCP Communication** between peers
- 🧠 **Fallback to Server** if peers don't have the file
- 🪪 **Token-based Authentication** to validate data origin
- 💡 **Keep-alive mechanism** to detect and remove inactive clients
- 🧵 Built with **multithreading (pthreads)**
- 🐧 Tested on **Ubuntu/Linux**

---
