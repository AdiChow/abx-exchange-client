#include <iostream>
#include <string>
#include <vector>
#include <array>    // Handy for fixed-size buffers
#include <map>      // Great for storing packets by sequence number, keeps them sorted!
#include <algorithm> // Useful for string trimming later
#include <fstream>  // Need this to write the final JSON file
#include <cstring>  // For memset, strerror, and memcpy if needed
#include <cerrno>   // So we can check errno when socket calls fail

// Alright, socket programming headers. Standard stuff for Linux/macOS.
#include <sys/socket.h>     // The core socket functions (socket, connect, send, recv, setsockopt)
#include <netinet/in.h>     // Structures for internet addresses (sockaddr_in, htons)
#include <arpa/inet.h>      // Functions for IP address conversion (inet_pton)
#include <unistd.h>         // For closing the socket file descriptor (close)
#include <sys/time.h>       // Gotta include this for struct timeval needed for SO_RCVTIMEO

// No external JSON library here, we're building that string by hand.

// Let's define what a packet looks like once we pull it off the wire.
struct Packet {
    std::string symbol;          // Like "MSFT" or "AAPL"
    char buysell_indicator;      // Should be 'B' or 'S'
    int32_t quantity;            // How many shares
    int32_t price;               // The price level
    int32_t sequence;            // The packet's unique sequence number

    // Little helper to print packet details, good for debugging!
    void print() const {
        std::cout << "  -> Seq: " << sequence
                  << ", Symbol: " << symbol
                  << ", Side: " << buysell_indicator
                  << ", Qty: " << quantity
                  << ", Price: " << price << std::endl;
    }
};

// The size of each packet is fixed, makes things easier.
const size_t PACKET_SIZE = 17; // 4 + 1 + 4 + 4 + 4 bytes
// Server details - standard localhost and port 3000.
const char* SERVER_HOST_IP = "127.0.0.1"; // Using IP directly for native connect
const int SERVER_PORT = 3000;            // Port number
// Setting a timeout so we don't hang forever if the server stops talking.
const int RECEIVE_TIMEOUT_SEC = 5;       // 5 seconds should be reasonable

// Function to take those raw bytes and turn them into our Packet struct.
// Gotta pay attention to the big-endian stuff here!
Packet parse_packet(const std::vector<char>& raw_data, size_t offset) {
    Packet packet;
    // Using unsigned char pointer helps avoid any weird signedness issues with raw bytes.
    const unsigned char* data = reinterpret_cast<const unsigned char*>(raw_data.data() + offset);

    // Pulling out the pieces based on the spec...
    // Symbol (4 bytes ASCII)
    // Just grab the bytes for now, we'll trim spaces/nulls when formatting JSON.
    packet.symbol.assign(reinterpret_cast<const char*>(data), 4);

    // Buy/Sell Indicator (1 byte ASCII)
    packet.buysell_indicator = data[4]; // Simple enough

    // Quantity (4 bytes int32, Big Endian)
    // This is the manual way to handle Big Endian - shifting bytes into place.
    packet.quantity = (static_cast<int32_t>(data[5]) << 24) |
                      (static_cast<int32_t>(data[6]) << 16) |
                      (static_cast<int32_t>(data[7]) << 8) |
                       static_cast<int32_t>(data[8]);

    // Price (4 bytes int32, Big Endian)
    packet.price = (static_cast<int32_t>(data[9]) << 24) |
                   (static_cast<int32_t>(data[10]) << 16) |
                   (static_cast<int32_t>(data[11]) << 8) |
                    static_cast<int32_t>(data[12]);

    // Packet Sequence (4 bytes int32, Big Endian)
    packet.sequence = (static_cast<int32_t>(data[13]) << 24) |
                      (static_cast<int32_t>(data[14]) << 16) |
                      (static_cast<int32_t>(data[15]) << 8) |
                       static_cast<int32_t>(data[16]);

    return packet;
}

int main() {
    // This map will hold all the packets we successfully receive, keyed by sequence number.
    // std::map is awesome here because it keeps everything sorted by key (sequence)!
    std::map<int32_t, Packet> received_packets;

    int initial_socket = -1; // Our socket file descriptor for the first connection
    struct sockaddr_in initial_server_addr; // Where the server lives

    try {
        // --- Stage 1 & 2: Connect and Ask for the Whole Stream ---

        // Step 1: Get a socket file descriptor
        initial_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (initial_socket == -1) {
            std::cerr << "Error creating initial socket! " << strerror(errno) << std::endl;
            return 1; // Can't do much without a socket
        }

        // Let's set a receive timeout right away so we don't block forever.
        struct timeval timeout;
        timeout.tv_sec = RECEIVE_TIMEOUT_SEC; // Our chosen timeout
        timeout.tv_usec = 0;

        if (setsockopt(initial_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout) < 0) {
            // It's usually okay if this fails, just means reads might block indefinitely if the server misbehaves.
            std::cerr << "Warning: Couldn't set receive timeout on initial socket. " << strerror(errno) << std::endl;
        } else {
             std::cout << "Set initial socket receive timeout to " << RECEIVE_TIMEOUT_SEC << " seconds." << std::endl;
        }


        // Step 2: Fill in the server's address details
        std::memset(&initial_server_addr, 0, sizeof(initial_server_addr)); // Clear it out first
        initial_server_addr.sin_family = AF_INET; // We're using IPv4
        // Convert the port number to network byte order - important!
        initial_server_addr.sin_port = htons(SERVER_PORT);

        // Convert the human-readable IP address string to the binary format the struct needs.
        if (inet_pton(AF_INET, SERVER_HOST_IP, &initial_server_addr.sin_addr) <= 0) {
            std::cerr << "Oops! Invalid address or address not supported: " << SERVER_HOST_IP << std::endl;
            close(initial_socket); // Clean up the socket we created
            return 1;
        }

        // Step 3: Time to connect!
        std::cout << "Attempting connection to " << SERVER_HOST_IP << ":" << SERVER_PORT << " for the initial stream..." << std::endl;
        if (connect(initial_socket, (struct sockaddr *)&initial_server_addr, sizeof(initial_server_addr)) < 0) {
            std::cerr << "Connection failed! " << strerror(errno) << std::endl;
            close(initial_socket);
            return 1;
        }
        std::cout << "Successfully connected for the initial stream!" << std::endl;

        // Now, send the request to get all packets. The spec says it's just 1 byte with value 1.
        unsigned char request_payload = 1; // Value 1 for Stream All Packets
        ssize_t bytes_sent = send(initial_socket, &request_payload, 1, 0); // Flags usually 0 for TCP

        if (bytes_sent == -1) {
            std::cerr << "Error sending 'Stream All Packets' request! " << strerror(errno) << std::endl;
            close(initial_socket);
            return 1;
        } else if (bytes_sent == 0) {
             std::cerr << "Connection closed by peer before sending the initial request?" << std::endl;
             close(initial_socket);
             return 1;
        } else if (bytes_sent < 1) {
             // This shouldn't really happen for 1 byte on a good connection, but good to check.
             std::cerr << "Warning: Sent " << bytes_sent << " bytes instead of 1 for the initial request." << std::endl;
             close(initial_socket); // Treat unexpected send as an error
             return 1;
        } else { // bytes_sent == 1
            std::cout << "Sent 'Stream All Packets' request (1 byte)." << std::endl;
        }


        // --- Stage 3: Receive and Process the Data Stream ---
        // We need a buffer to collect data as it comes in, since TCP is a stream and data might be chunked.
        std::vector<char> receive_buffer;
        // A temporary buffer to read chunks from the socket into before appending to the main buffer.
        std::array<char, 1024> temp_buffer;

        std::cout << "Receiving initial data stream..." << std::endl;

        ssize_t bytes_read;
        // Loop to keep reading data until the server closes the connection (recv returns 0),
        // an error occurs (-1 with non-timeout errno), or our timeout hits (-1 with EAGAIN/EWOULDBLOCK).
        while (true) {
            bytes_read = recv(initial_socket, temp_buffer.data(), temp_buffer.size(), 0);

            if (bytes_read > 0) {
                // Got some data! Append it to our collection buffer.
                receive_buffer.insert(receive_buffer.end(), temp_buffer.begin(), temp_buffer.begin() + bytes_read);

                // Now, let's see if we have any complete packets in our buffer.
                while (receive_buffer.size() >= PACKET_SIZE) {
                    // Yes! We have at least one full packet. Parse the first one.
                    Packet packet = parse_packet(receive_buffer, 0);

                    // Store it in our map. The sequence number is the key.
                    received_packets[packet.sequence] = packet;
                    // Optional: See the packet details as we get them.
                    // packet.print();

                    // Remove the bytes we just processed from the front of the buffer.
                    receive_buffer.erase(receive_buffer.begin(), receive_buffer.begin() + PACKET_SIZE);
                    // The inner while loop continues to check if there's *another* full packet right after this one.
                }
            } else if (bytes_read == 0) {
                // recv returning 0 means the server closed the connection gracefully.
                std::cout << "Server closed the initial connection gracefully." << std::endl;
                break; // We're done with the initial stream
            } else { // bytes_read == -1
                // An error or timeout occurred. Check errno.
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // This is the timeout we set! The server stopped sending data within the time limit.
                    std::cerr << "Receive timeout reached for initial data stream. Proceeding with received data." << std::endl;
                     // Break the loop here. We'll process any complete packets we got before the timeout.
                     break;
                } else {
                    // A different kind of error happened during receiving.
                    std::cerr << "A non-timeout error occurred during initial receiving: " << strerror(errno) << std::endl;
                    // This might mean the connection broke unexpectedly.
                    // For this test, we'll proceed with the data received up to this point.
                    break; // Exit receive loop
                }
            }
        }

        // Done with the initial connection. Close the socket file descriptor.
        close(initial_socket);
        initial_socket = -1; // Mark as closed so we don't try to close it again.

        std::cout << "Finished the initial data stream phase. Collected " << received_packets.size() << " packets so far." << std::endl;
        // Note: If a timeout happened, we might not have received all packets from the initial stream.

        // --- Stage 4 & 5: Find Missing Packets and Ask for Resends ---

        // Find the highest sequence number we received. The problem guarantees the last one isn't missed in the *full* set.
        int32_t max_sequence = 0;
        if (!received_packets.empty()) {
            max_sequence = received_packets.rbegin()->first; // rbegin() points to the last element (highest key) in the sorted map
        }
        std::cout << "Highest sequence number found in initial stream: " << max_sequence << std::endl;

        // Build a list of all the sequence numbers we *should* have, but didn't get.
        std::vector<int32_t> missing_sequences;
        for (int32_t i = 1; i <= max_sequence; ++i) {
            if (received_packets.find(i) == received_packets.end()) {
                // If 'i' isn't a key in our map, it's missing!
                missing_sequences.push_back(i);
                // std::cout << "Hmm, sequence " << i << " seems to be missing." << std::endl; // Can get chatty if many are missing
            }
        }
        std::cout << "Identified " << missing_sequences.size() << " missing sequences that need resending." << std::endl;

        // Time to go fetch those missing packets, one by one.
        for (int32_t seq_to_resend : missing_sequences) {
            std::cout << "Requesting resend for sequence: " << seq_to_resend << std::endl;

            int resend_socket = -1; // A new socket for each resend request
            struct sockaddr_in resend_server_addr; // Server address again

            try {
                // Step 1: Need a brand new connection for this resend.
                resend_socket = socket(AF_INET, SOCK_STREAM, 0);
                if (resend_socket == -1) {
                    std::cerr << "  Error creating resend socket for seq " << seq_to_resend << "! " << strerror(errno) << std::endl;
                    continue; // Skip this one and try the next missing seq
                }

                // Set the receive timeout for this resend socket too.
                // Use the same timeout for consistency.
                if (setsockopt(resend_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout) < 0) {
                    std::cerr << "  Warning: Could not set receive timeout on resend socket for seq " << seq_to_resend << ". " << strerror(errno) << std::endl;
                } else {
                     std::cout << "  Set resend socket receive timeout to " << RECEIVE_TIMEOUT_SEC << " seconds." << std::endl;
                }


                // Step 2: Server address details are the same.
                std::memset(&resend_server_addr, 0, sizeof(resend_server_addr));
                resend_server_addr.sin_family = AF_INET;
                resend_server_addr.sin_port = htons(SERVER_PORT);

                if (inet_pton(AF_INET, SERVER_HOST_IP, &resend_server_addr.sin_addr) <= 0) {
                    std::cerr << "  Invalid address/ Address not supported for resend: " << SERVER_HOST_IP << std::endl;
                    close(resend_socket);
                    continue;
                }

                // Connect for this resend request.
                std::cout << "  Connecting for resend request..." << std::endl;
                if (connect(resend_socket, (struct sockaddr *)&resend_server_addr, sizeof(resend_server_addr)) < 0) {
                    std::cerr << "  Resend connection failed for seq " << seq_to_resend << "! " << strerror(errno) << std::endl;
                    close(resend_socket);
                    continue;
                }
                std::cout << "  Successfully connected for resend." << std::endl;

                // Now, build and send the resend request payload.
                // Spec says it's 2 bytes: Call Type 2 (Resend) + the sequence number (1 byte).
                // Note: The server reads the sequence number as an Int8 (1 byte).
                // If sequence numbers go above 127/255, the server might misinterpret them.
                // Assuming for this test they stay within 1-byte range based on sample data.
                 if (seq_to_resend < 0 || seq_to_resend > 255) {
                      std::cerr << "  Warning: Sequence number " << seq_to_resend << " is outside the usual 1-byte range (0-255) for the resend payload. The server might have trouble with this." << std::endl;
                 }
                unsigned char resend_payload[2] = {2, static_cast<unsigned char>(seq_to_resend)}; // Value 2 for Resend Packet

                // Send the 2-byte request.
                ssize_t bytes_sent_resend = send(resend_socket, resend_payload, 2, 0);

                if (bytes_sent_resend == -1) {
                    std::cerr << "  Error sending resend request for seq " << seq_to_resend << "! " << strerror(errno) << std::endl;
                     close(resend_socket);
                     continue;
                } else if (bytes_sent_resend == 2) {
                     std::cout << "  Sent resend request payload." << std::endl;
                 } else {
                     std::cerr << "  Warning: Sent " << bytes_sent_resend << " bytes instead of 2 for resend request for seq " << seq_to_resend << "." << std::endl;
                     close(resend_socket); // Treat unexpected send as error
                     continue;
                 }


                // Now, we expect exactly ONE packet (17 bytes) back from the server for this resend.
                std::vector<char> resent_packet_data(PACKET_SIZE); // Buffer just for this single packet
                size_t total_bytes_received = 0;
                ssize_t current_bytes_read;

                // Loop carefully to make sure we get all 17 bytes, handling partial reads and the timeout.
                while (total_bytes_received < PACKET_SIZE) {
                    current_bytes_read = recv(resend_socket,
                                              // Read into the buffer starting from where we left off
                                              resent_packet_data.data() + total_bytes_received,
                                              // Only ask for the bytes we still need
                                              PACKET_SIZE - total_bytes_received,
                                              0);

                    if (current_bytes_read == -1) {
                        // Error or timeout on this receive call.
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                             std::cerr << "  Receive timeout hit while getting resent packet for seq " << seq_to_resend << ". Didn't get the full packet." << std::endl;
                             // Indicate failure to get the complete packet.
                             total_bytes_received = 0;
                        } else {
                             // A different receive error.
                             std::cerr << "  Error receiving resent packet for seq " << seq_to_resend << "! " << strerror(errno) << std::endl;
                             total_bytes_received = 0; // Indicate failure
                        }
                        break; // Exit the inner while loop for this packet
                    } else if (current_bytes_read == 0) {
                        // Connection closed by server unexpectedly before sending the whole packet?
                        std::cerr << "  Server closed connection prematurely while getting resent packet for seq " << seq_to_resend << ". Expected " << PACKET_SIZE << " bytes, but only got " << total_bytes_received << " so far." << std::endl;
                        total_bytes_received = 0; // Indicate failure
                        break; // Exit the inner while loop
                    } else {
                        // Got some bytes, update the total.
                        total_bytes_received += current_bytes_read;
                    }
                }

                // If we successfully got all 17 bytes...
                if (total_bytes_received == PACKET_SIZE) {
                     std::cout << "  Got the resent packet (" << total_bytes_received << " bytes)." << std::endl;

                    // Parse those 17 bytes into a Packet struct.
                    Packet resent_packet = parse_packet(resent_packet_data, 0);

                    // Quick check: Is this the packet we actually asked for?
                    if (resent_packet.sequence != seq_to_resend) {
                        std::cerr << "  Warning: Requested seq " << seq_to_resend << " but received packet has seq " << resent_packet.sequence << ". Data might be mixed up or corrupted." << std::endl;
                        // We'll still store it by its *reported* sequence, but this is suspicious.
                    }

                    // Add/update this packet in our main collection.
                    received_packets[resent_packet.sequence] = resent_packet;
                    std::cout << "  Successfully added/updated sequence: " << resent_packet.sequence << " in our collection." << std::endl;
                    // Optional: print the resent packet details.
                    // resent_packet.print();

                } // else: An error or timeout happened while receiving the resent packet, already handled/logged.


            } catch (const std::exception& e) {
                 // Catch any unexpected exceptions during the resend process for this sequence.
                std::cerr << "  An unexpected issue came up during resend for seq " << seq_to_resend << ": " << e.what() << std::endl;
            }

             // IMPORTANT: Close this resend connection! The spec says it's the client's job for Call Type 2.
             // This needs to happen outside the inner try-catch to ensure it runs even if an exception happened,
             // as long as the socket was successfully created before the exception.
             if (resend_socket != -1) { // Check if the socket was valid to begin with
                  close(resend_socket);
                  resend_socket = -1; // Mark it as closed
                  std::cout << "  Closed connection after resend." << std::endl;
             }
        }
        std::cout << "Finished trying to fetch missing packets. Total packets collected now: " << received_packets.size() << std::endl;

        // --- Stage 6: Build and Write the Final JSON Output ---
        std::cout << "Okay, all packets collected (hopefully!). Let's build that JSON file." << std::endl;

        std::string json_output_string = "[\n"; // JSON array starts here

        bool first_packet = true;
        // Iterate through our map. It's already sorted by sequence number, which is exactly what we need for the JSON array order.
        for (const auto& pair : received_packets) {
            const Packet& packet = pair.second; // The actual packet data

            // Add a comma and newline before adding the next object, unless it's the very first one.
            if (!first_packet) {
                json_output_string += ",\n";
            }
            first_packet = false; // After the first one, this will always be false.

            // Start of a JSON object for this packet, add some indentation for readability ("pretty printing").
            json_output_string += "    {\n"; // Indent 4 spaces

            // Add the key-value pairs for the packet.
            // Remember to put quotes around keys and string values! Numbers don't get quotes.
            // Also, trim any trailing spaces/nulls from the symbol string.
            std::string symbol_str = packet.symbol;
            // Find the position of the last character that is NOT a space or a null terminator.
            size_t last_char_pos = symbol_str.find_last_not_of(" \0");
            if (std::string::npos != last_char_pos) {
                // If we found a non-whitespace/non-null character, trim the string up to that point.
                symbol_str = symbol_str.substr(0, last_char_pos + 1);
            } else {
                // If the string was all spaces or nulls, make it an empty string for JSON.
                symbol_str.clear();
            }

            json_output_string += "        \"symbol\": \"" + symbol_str + "\",\n"; // Symbol is a string
            // Indicator is a single character, represent it as a string in JSON.
            json_output_string += "        \"buysell_indicator\": \"" + std::string(1, packet.buysell_indicator) + "\",\n";
            // Quantity, price, and sequence are numbers, use std::to_string to convert.
            json_output_string += "        \"quantity\": " + std::to_string(packet.quantity) + ",\n";
            json_output_string += "        \"price\": " + std::to_string(packet.price) + ",\n";
            json_output_string += "        \"packetSequence\": " + std::to_string(packet.sequence) + "\n"; // No comma after the last field in the object

            // End of the JSON object for this packet.
            json_output_string += "    }";
        }

        json_output_string += "\n]\n"; // End of the JSON array

        // Write the whole JSON string to the output file.
        std::ofstream output_file("output.json");
        if (output_file.is_open()) {
            output_file << json_output_string;
            output_file.close();
            std::cout << "Success! Output written to output.json" << std::endl;
        } else {
            std::cerr << "Boo! Couldn't open output.json for writing. " << strerror(errno) << std::endl;
            return 1; // Indicate failure
        }

    } catch (const std::exception& e) {
        // Catch any major exceptions that somehow slipped through (unlikely with careful error handling).
        std::cerr << "An unexpected critical error occurred: " << e.what() << std::endl;
         // Just in case, try to close the initial socket if it was somehow left open.
        if (initial_socket != -1) {
             close(initial_socket);
        }
        return 1; // Indicate failure
    }

    // If we got this far, everything completed successfully!
    return 0;
}