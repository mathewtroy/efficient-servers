#include <arpa/inet.h>
#include <fstream>
#include <iostream>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "server.pb.h"

int main() {
    std::ifstream file("out.bin", std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open out.bin\n";
        return 1;
    }

    int response_id = 0;
    int total = 0;
    int skipped = 0;

    while (true) {
        uint32_t len_be = 0;
        file.read(reinterpret_cast<char*>(&len_be), sizeof(len_be));
        if (!file) {
            break;
        }

        const uint32_t len = ntohl(len_be);
        std::vector<uint8_t> buffer(len);

        if (len > 0) {
            file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(len));
            if (!file) {
                std::cerr << "Unexpected end of file while reading payload\n";
                return 1;
            }
        }

        Response resp;
        if (len > 0 && !resp.ParseFromArray(buffer.data(), static_cast<int>(len))) {
            std::cerr << "Failed to parse Response\n";
            return 1;
        }

        total++;
        response_id++;

        const auto* descriptor = resp.GetDescriptor();
        const auto* reflection = resp.GetReflection();

        const auto* err_field = descriptor->FindFieldByName("errMsg");
        const auto* shortest_field = descriptor->FindFieldByName("shortest_path_length");
        const auto* total_field = descriptor->FindFieldByName("total_length");

        // Filter
        bool has_meaningful = false;

        if (err_field != nullptr && reflection->HasField(resp, err_field)) {
            has_meaningful = true;
        }
        if (shortest_field != nullptr && reflection->HasField(resp, shortest_field)) {
            has_meaningful = true;
        }
        if (total_field != nullptr && reflection->HasField(resp, total_field)) {
            has_meaningful = true;
        }

        if (!has_meaningful) {
            skipped++;
            continue;
        }

        // Print useful responses
        std::cout << "Response " << response_id << ":\n";

        if (resp.status() == Response::OK) {
            std::cout << "  status: OK\n";
        } else {
            std::cout << "  status: ERROR\n";
        }

        if (err_field != nullptr && reflection->HasField(resp, err_field)) {
            std::cout << "  errMsg: " << resp.errmsg() << "\n";
        }

        if (shortest_field != nullptr && reflection->HasField(resp, shortest_field)) {
            std::cout << "  shortest_path_length: "
                      << resp.shortest_path_length() << "\n";
        }

        if (total_field != nullptr && reflection->HasField(resp, total_field)) {
            std::cout << "  total_length: "
                      << resp.total_length() << "\n";
        }

        std::cout << "\n";
    }

    std::cout << "==============================\n";
    std::cout << "Total responses: " << total << "\n";
    std::cout << "Skipped (empty OK): " << skipped << "\n";
    std::cout << "Meaningful: " << (total - skipped) << "\n";

    return 0;
}