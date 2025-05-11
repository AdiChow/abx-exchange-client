# ABX Exchange Client

This project implements a C++ client application designed to interact with a mock ABX Exchange Server. The client connects to the server, requests stock ticker data, handles potential missing data packets, and outputs the complete, ordered data stream to a JSON file.

This client was developed as part of a take-home test.

## Prerequisites

Before running the server or compiling/running the client, ensure you have the following installed:

* **Git:** For cloning or managing the project.
* **Node.js:** Version 16.17.0 or higher is required for the mock server.
* **C++ Compiler:** A C++11 compliant compiler (like g++, clang++, or MSVC) is needed to build the client application. This client uses Native POSIX Sockets, so the provided compilation instructions are tailored for Unix-like systems (Linux, macOS).

## 1. Running the ABX Exchange Server

The mock exchange server is provided as a Node.js application.

1.  Navigate to the directory containing the server file (`main.js`). This file is expected to be within a folder like `abx_exchange_server/`.
2.  Open your terminal or command prompt in that directory.
3.  Run the server using Node.js:
    ```bash
    node main.js
    ```
4.  The server should start and indicate it's listening on port 3000. Keep this terminal open and the server running while you use the client.

## 2. Compiling the C++ Client

The client code is a single C++ source file.

1.  Navigate to the directory containing the client source file (`client.cpp`).
2.  Open your terminal or command prompt in that directory.
3.  Compile the code using a C++ compiler. For g++ on a Unix-like system, use a command like this:
    ```bash
    g++ client.cpp -o client -std=c++11 -Wall -Wextra
    ```
    * `client.cpp`: This is your client source file.
    * `-o client`: Names the output executable `client`. You can change this name if you like, but remember to update the run command accordingly.
    * `-std=c++11`: Compiles using the C++11 standard (you can use a higher standard like `c++14`, `c++17`, etc., if preferred).
    * `-Wall -Wextra`: Enables recommended compiler warnings.

4.  If compilation is successful, an executable file (e.g., `client`) will be created in the current directory.

## 3. Running the Client and Viewing Output

Once the server is running and the client is compiled, you can run the client to fetch data.

1.  Ensure the **ABX Exchange Server is currently running** in a separate terminal window.
2.  Open a new terminal window and navigate to the directory containing the compiled client executable (e.g., `client`).
3.  Run the client executable:
    ```bash
    ./client
    ```
4.  The client will print progress messages to the console as it connects, requests data, receives packets (including handling missing ones), and generates the output.
5.  Upon successful completion, the client will create a JSON file named `output.json` in the same directory where the client executable was run.
6.  To view the collected data, open the `output.json` file using any text editor. You can also view it in the terminal using commands like `cat output.json` or `less output.json`.

The `output.json` file will contain a JSON array of objects, where each object represents a stock ticker data packet, ordered by its increasing sequence number.
