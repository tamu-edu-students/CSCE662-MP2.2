// NOTE: This starter code contains a primitive implementation using the default RabbitMQ protocol.
// You are recommended to look into how to make the communication more efficient,
// for example, modifying the type of exchange that publishes to one or more queues, or
// throttling how often a process consumes messages from a queue so other consumers are not starved for messages
// All the functions in this implementation are just suggestions and you can make reasonable changes as long as
// you continue to use the communication methods that the assignment requires between different processes

#include <bits/fs_fwd.h>
#include <ctime>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <stdio.h>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#include "sns.grpc.pb.h"
#include "sns.pb.h"
#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <jsoncpp/json/json.h>

#define log(severity, msg) \
    LOG(severity) << msg;  \
    google::FlushLogFiles(google::severity);

namespace fs = std::filesystem;

using csce662::AllUsers;
using csce662::Confirmation;
using csce662::CoordService;
using csce662::ID;
using csce662::ServerInfo;
using csce662::SynchIDs;
using csce662::ServerList;
using csce662::SynchronizerListReply;
using csce662::SynchService;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
// tl = timeline, fl = follow list
using csce662::TLFL;

int synchID = 1;
int clusterID = 1;
bool isMaster = false;
int total_number_of_registered_synchronizers = 6; // update this by asking coordinator
std::string coordAddr;
std::string clusterSubdirectory;
std::vector<std::string> otherHosts;
std::unordered_map<std::string, int> timelineLengths;

std::vector<std::string> get_lines_from_file(std::string);
std::vector<std::string> get_all_users_func(int);
std::vector<std::string> get_tl_or_fl(int, int, bool);
std::vector<std::string> getFollowersOfUser(int);
bool file_contains_user(std::string filename, std::string user);

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID);

bool IsMaster();

std::vector<std::string> getFollowingsOfUser(int);

bool clientInCluster(int);

std::vector<int> GetOtherClusterSynchIDs();

std::unique_ptr<csce662::CoordService::Stub> stub;


std::unordered_map<std::string, int> file_size;

std::uintmax_t getFileSize(const std::string& filename) {
    std::filesystem::path filePath(filename);
    if (!std::filesystem::exists(filePath)) {
        throw std::runtime_error("File does not exist: " + filename);
    }
    return std::filesystem::file_size(filePath);
}

std::unordered_map<std::string, int> line_number;



struct PairHash {
    std::size_t operator()(const std::pair<std::string, std::string>& p) const {
        std::size_t h1 = std::hash<std::string>{}(p.first);  // Hash the first element
        std::size_t h2 = std::hash<std::string>{}(p.second); // Hash the second element
        return h1 ^ (h2 << 1); // Combine the hashes using XOR and a shift
    }
};

std::unordered_map<std::pair<std::string, std::string>, int, PairHash> already_sent_message_to_queue;


class SynchronizerRabbitMQ
{
private:
    amqp_connection_state_t conn;
    amqp_channel_t channel;
    std::string hostname;
    int port;
    int synchID;

    void setupRabbitMQ()
    {
        conn = amqp_new_connection();
        amqp_socket_t *socket = amqp_tcp_socket_new(conn);
        amqp_socket_open(socket, hostname.c_str(), port);
        amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "guest", "guest");
        amqp_channel_open(conn, channel);
    }

    void declareQueue(const std::string &queueName)
    {
        amqp_queue_declare(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           0, 0, 0, 0, amqp_empty_table);
    }

    void publishMessage(const std::string &queueName, const std::string &message)
    {
        auto key = std::make_pair(queueName, message);
        if (already_sent_message_to_queue.find(key) == already_sent_message_to_queue.end()) {
            already_sent_message_to_queue[key] = 0;
        }

        if (already_sent_message_to_queue[key] < 3) {
            amqp_basic_publish(conn, channel, amqp_empty_bytes, amqp_cstring_bytes(queueName.c_str()),
                           0, 0, NULL, amqp_cstring_bytes(message.c_str()));
            already_sent_message_to_queue[key] += 1;
        }
    }

    std::string consumeMessage(const std::string &queueName, int timeout_ms = 5000)
    {
        amqp_basic_consume(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           amqp_empty_bytes, 0, 1, 0, amqp_empty_table);

        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(conn);

        struct timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        amqp_rpc_reply_t res = amqp_consume_message(conn, &envelope, &timeout, 0);

        if (res.reply_type != AMQP_RESPONSE_NORMAL)
        {
            return "";
        }

        std::string message(static_cast<char *>(envelope.message.body.bytes), envelope.message.body.len);
        amqp_destroy_envelope(&envelope);
        return message;
    }

public:
    SynchronizerRabbitMQ(const std::string &host, int p, int id) : hostname(host), port(p), channel(1), synchID(id)
    {
        setupRabbitMQ();
        declareQueue("synch" + std::to_string(synchID) + "_users_queue");
        declareQueue("synch" + std::to_string(synchID) + "_clients_relations_queue");
        declareQueue("synch" + std::to_string(synchID) + "_timeline_queue");
        // TODO: add or modify what kind of queues exist in your clusters based on your needs
    }

    void publishUserList(int other_synchID)
    {
        std::vector<std::string> users = get_all_users_func(synchID);
        std::sort(users.begin(), users.end());
        Json::Value userList;
        for (const auto &user : users)
        {
            userList["users"].append(user);
        }
        Json::FastWriter writer;
        std::string message = writer.write(userList);
        // std::cout << "From synchID " << synchID << " to " << other_synchID << " with users " << message << std::endl;
        publishMessage("synch" + std::to_string(other_synchID) + "_users_queue", message);
    }

    void consumeUserLists()
    {
        std::vector<std::string> allUsers;
        // YOUR CODE HERE

        // TODO: while the number of synchronizers is harcorded as 6 right now, you need to change this
        // to use the correct number of follower synchronizers that exist overall
        // accomplish this by making a gRPC request to the coordinator asking for the list of all follower synchronizers registered with it

        std::string queueName = "synch" + std::to_string(synchID) + "_users_queue";
        std::string message = consumeMessage(queueName, 100); // 1 second timeout
        // std::cout << "In synchronizer " << synchID << " recieved " << message << std::endl;
        if (!message.empty())
        {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(message, root))
            {
                for (const auto &user : root["users"])
                {
                    allUsers.push_back(user.asString());
                }
            }
            updateAllUsersFile(allUsers);
        }
    }

    void publishClientRelations(int other_synchID)
    {
        Json::Value relations;
        std::vector<std::string> users = get_all_users_func(synchID);

        for (const auto &client : users)
        {
            int clientId = std::stoi(client);

            if (clientInCluster(clientId)) {
                // std::cout << "Cluster " << clusterID << " has client: " << clientId << std::endl;
                std::vector<std::string> followings = getFollowingsOfUser(clientId);
                
                // std::cout << "Client follows: " << std::endl;
                // for (auto following : followings) {
                //     std::cout << following << " ";
                // }
                // std::cout << "" << std::endl;
                
                Json::Value followingList(Json::arrayValue);
                for (const auto &following : followings)
                {
                    followingList.append(following);
                }

                if (!followingList.empty())
                {
                    relations[client] = followingList;
                }
            }
        }

        Json::FastWriter writer;
        std::string message = writer.write(relations);
        // std::cout << "Cluster id: " << clusterID << " with message: " << message << std::endl;
        publishMessage("synch" + std::to_string(other_synchID) + "_clients_relations_queue", message);
    }

    void consumeClientRelations()
    {
        std::vector<std::string> allUsers = get_all_users_func(synchID);

        std::string queueName = "synch" + std::to_string(synchID) + "_clients_relations_queue";
        std::string message = consumeMessage(queueName, 100); // 1 second timeout

        if (!message.empty())
        {   
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(message, root))
            {
                // std::cout << "Synchronizer with ID " << synchID << " and msg: " << message << std::endl;
                for (const auto &follower : allUsers) {
                    if (root.isMember(follower)) {
                        for (const auto &leader : root[follower]) {
                            if (clientInCluster(std::stoi(leader.asString()))) {
                                std::string filename = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + leader.asString() + "_followers.txt";
                                std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + leader.asString() + "_followers.txt";
                                sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
                                std::ofstream file(filename, std::ios::app | std::ios::out | std::ios::in);
                                if (!file_contains_user(filename, follower)) {
                                    file << follower << std::endl;
                                }
                                sem_close(fileSem);
                            }
                        }
                    }
                }
            }
        }
    }

    // for every client in your cluster, update all their followers' timeline files
    // by publishing your user's timeline file (or just the new updates in them)
    //  periodically to the message queue of the synchronizer responsible for that client
    void publishTimelines(int other_synchID)
    {
        std::vector<std::string> users = get_all_users_func(synchID);

        for (const auto &client : users) {
            int clientId = std::stoi(client);
            if (clientInCluster(clientId)) {                
                std::string writeFilename = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + client + "_write.txt";

                if (file_size.find(writeFilename) == file_size.end()) file_size[writeFilename] = 0;
                if (line_number.find(writeFilename) == line_number.end()) line_number[writeFilename] = 0;

                std::ifstream writeFile(writeFilename);
                uintmax_t currentFileSize = getFileSize(writeFilename);
                if (writeFile.is_open() && currentFileSize > file_size[writeFilename]) {
                    
                    file_size[writeFilename] = currentFileSize;
                    int start_line_number = line_number[writeFilename];
                    int current_line_number = 0;
                    std::string message;
                    // Json::Value new_timeline_messages(Json::arrayValue);
                    std::vector<std::string> new_timeline_messages;
                    
                    while (std::getline(writeFile, message)) {
                        if (current_line_number >= start_line_number) {
                            // do something with message
                            // {"t2" : ["message1", "message2", ... ""]}
                            new_timeline_messages.push_back(message);
                        }
                        current_line_number += 1;
                    }
                    line_number[writeFilename] = current_line_number;

                    if (new_timeline_messages.empty()) assert(false);
                    
                    // else {
                    //     timeline["t" + client] = new_timeline_messages;
                    // }

                    // Json::FastWriter writer;
                    // message = writer.write(timeline);
                    // std::cout << "Timeline Message: " << message << std::endl;

                    std::vector<std::string> followers = getFollowersOfUser(clientId);

                    for (const auto &follower : followers) {
                        // send the timeline updates of your current user to all its followers

                        // YOUR CODE HERE
                        // print the follower and the message to be sent to it
                        
                        // Json::Value timeline;
                        // timeline[follower] = new_timeline_messages;
                        // Json::FastWriter writer;
                        // message = writer.write(timeline);

                        // std::cout << "To follower: " << follower << " message: " << message << std::endl;

                        int recipient_cluster = ((std::stoi(follower) - 1) % 3) + 1;

                        std::string filename = "./cluster_" + std::to_string(recipient_cluster) + "/" + "1" + "/" + follower + "_read.txt";
                        std::ofstream file(filename, std::ios::app);
                        if (!file) { std::cerr << "Error: Unable to open the file: " << filename << std::endl; return; }
                        for (auto &message : new_timeline_messages) {
                            file << message << "\n";
                        }
                        file.close();


                        std::string filename2 = "./cluster_" + std::to_string(recipient_cluster) + "/" + "2" + "/" + follower + "_read.txt";
                        std::ofstream file2(filename2, std::ios::app);
                        if (!file2) { std::cerr << "Error: Unable to open the file: " << filename2 << std::endl; return; }
                        for (auto &message : new_timeline_messages) {
                            file2 << message << "\n";
                        }
                        file2.close();

                        // int follower_queue = ((std::stoi(follower) - 1) % 3) + 1;
                        // publishMessage("synch" + std::to_string(other_synchID) + "_timeline_queue", message);



                    }
                }
            }
        }
    }

    // For each client in your cluster, consume messages from your timeline queue and modify your client's timeline files based on what the users they follow posted to their timeline
    void consumeTimelines()
    {
        std::string queueName = "synch" + std::to_string(synchID) + "_timeline_queue";
        std::string message = consumeMessage(queueName, 1000); // 1 second timeout

        if (!message.empty())
        {
            // consume the message from the queue and update the timeline file of the appropriate client with
            // the new updates to the timeline of the user it follows

            // YOUR CODE HERE
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(message, root))
            {
                std::cout << "Synchronizer with ID " << synchID << " and msg: " << message << std::endl;
                // std::vector<std::string> allUsers = get_all_users_func(synchID);
                // for (const auto& recipient : allUsers) {
                //     if (root.isMember("t" + recipient) && clientInCluster(std::stoi(recipient))) {
                //         std::cout << "here! " << recipient << std::endl;
                //         std::string filename = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + recipient + "_read.txt";
                //         std::ofstream file(filename, std::ios::app);
                //         if (!file) { std::cerr << "Error: Unable to open the file: " << filename << std::endl; return; }
                //         for (const auto &message : root["t" + recipient]) {
                //             file << message.asString() << std::endl;
                //         }
                //         file.close();
                //     }
                // }

                // for (const auto& t_recipient : root.getMemberNames()) {
                //     // std::cout << "Key: " << t_recipient << " Value: " << root[t_recipient] << std::endl;
                //     // std::string recipient = t_recipient.at(1);
                    
                // }

                // Iterate through the keys and values
                for (const auto& recipient : root.getMemberNames()) {
                    // Check if the key starts with "t"
                    // if (t_recipient.rfind("t", 0) == 0) {  // rfind("t", 0) checks if it starts with "t"
                    //     // Get the substring after "t"
                    //     std::string recipient = t_recipient.substr(1);  // Skip the "t"
                    //     std::cout << "Key: " << t_recipient << " (substring after 't'): " << recipient << " Value: " << root[t_recipient] << std::endl;
                    // }
                    std::cout << "Key: " << recipient << " Value: " << root[recipient] << std::endl;
                    if (root[recipient].isArray()) {
                        std::cout << "Is Array!" << std::endl;
                        if (root[recipient].size() > 0) {
                            std::cout << "at least one element!" << std::endl;
                            if (root[recipient][0].size() > 3) {
                                std::cout << "timeline message!" << std::endl;
                            }
                        }
                    }
                    
                }

            }
        }
    }

private:
    void updateAllUsersFile(const std::vector<std::string> &users)
    {

        std::string usersFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/all_users.txt";
        std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_all_users.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

        std::ofstream userStream(usersFile, std::ios::app | std::ios::out | std::ios::in);
        for (std::string user : users)
        {
            if (!file_contains_user(usersFile, user))
            {
                userStream << user << std::endl;
            }
        }
        sem_close(fileSem);
    }
};

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ);

class SynchServiceImpl final : public SynchService::Service
{
    // You do not need to modify this in any way
};

void RunServer(std::string coordIP, std::string coordPort, std::string port_no, int synchID)
{
    // localhost = 127.0.0.1
    std::string server_address("127.0.0.1:" + port_no);
    log(INFO, "Starting synchronizer server at " + server_address);
    SynchServiceImpl service;
    // grpc::EnableDefaultHealthCheckService(true);
    // grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Initialize RabbitMQ connection
    SynchronizerRabbitMQ rabbitMQ("localhost", 5672, synchID);

    std::thread t1(run_synchronizer, coordIP, coordPort, port_no, synchID, std::ref(rabbitMQ));

    // Create a consumer thread
    std::thread consumerThread([&rabbitMQ]()
                               {
        while (true) {
            rabbitMQ.consumeUserLists();
            rabbitMQ.consumeClientRelations();
            // rabbitMQ.consumeTimelines();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            // you can modify this sleep period as per your choice
        } });

    server->Wait();

    //   t1.join();
    //   consumerThread.join();
}

int main(int argc, char **argv)
{
    int opt = 0;
    std::string coordIP;
    std::string coordPort;
    std::string port = "3029";

    while ((opt = getopt(argc, argv, "h:k:p:i:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            coordIP = optarg;
            break;
        case 'k':
            coordPort = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'i':
            synchID = std::stoi(optarg);
            break;
        default:
            std::cerr << "Invalid Command Line Argument\n";
        }
    }

    std::string log_file_name = std::string("synchronizer-") + port;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Server starting...");

    coordAddr = coordIP + ":" + coordPort;
    clusterID = ((synchID - 1) % 3) + 1;
    ServerInfo serverInfo;
    serverInfo.set_hostname("localhost");
    serverInfo.set_port(port);
    serverInfo.set_type("synchronizer");
    serverInfo.set_serverid(synchID);
    serverInfo.set_clusterid(clusterID);
    Heartbeat(coordIP, coordPort, serverInfo, synchID);

    RunServer(coordIP, coordPort, port, synchID);
    return 0;
}

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ)
{
    // setup coordinator stub
    // std::string target_str = coordIP + ":" + coordPort;
    // std::unique_ptr<CoordService::Stub> coord_stub_;
    // coord_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials())));

    ServerInfo msg;
    Confirmation c;

    msg.set_serverid(synchID);
    msg.set_hostname("127.0.0.1");
    msg.set_port(port);
    msg.set_type("follower");

    // TODO: begin synchronization process
    while (true)
    {
        // the synchronizers sync files every 5 seconds
        sleep(5);

        grpc::ClientContext context;
        ServerList followerServers;
        ID id;
        id.set_id(synchID);

        // std::cout << "Synchronizer with synchID " << synchID << " in main" << std::endl;

        bool _isMaster = IsMaster();
        if (!_isMaster && synchID <= 3) {
            std::cout << "Synchronizer with synchID " << synchID << " exiting" << std::endl;
            exit(1);
        }

        if (_isMaster) {
            std::vector<int> other_cluster_synchIDs = GetOtherClusterSynchIDs();

            // std::cout << "Synchronizer with synchID " << synchID << " is the master sending to: ";
            // for (auto id : other_cluster_synchIDs) {
            //     std::cout << id << " ";
            // }
            // std::cout << "" << std::endl;


            // assert(other_cluster_synchIDs.size() == 4);
            for (auto other_synchID : other_cluster_synchIDs) {
                // Publish user list
                rabbitMQ.publishUserList(other_synchID);

                // Publish client relations
                rabbitMQ.publishClientRelations(other_synchID);

                // Publish timelines
                rabbitMQ.publishTimelines(other_synchID);
            }
        }
    }
    return;
}

std::vector<std::string> get_lines_from_file(std::string filename)
{
    std::vector<std::string> users;
    std::string user;
    std::ifstream file;
    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + filename;
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
    file.open(filename);
    if (file.peek() == std::ifstream::traits_type::eof())
    {
        // return empty vector if empty file
        // std::cout<<"returned empty vector bc empty file"<<std::endl;
        file.close();
        sem_close(fileSem);
        return users;
    }
    while (file)
    {
        getline(file, user);

        if (!user.empty())
            users.push_back(user);
    }

    file.close();
    sem_close(fileSem);

    return users;
}

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID)
{
    // For the synchronizer, a single initial heartbeat RPC acts as an initialization method which
    // servers to register the synchronizer with the coordinator and determine whether it is a master

    log(INFO, "Sending initial heartbeat to coordinator");
    std::string coordinatorInfo = coordinatorIp + ":" + coordinatorPort;
    // std::unique_ptr<CoordService::Stub> stub = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(coordinatorInfo, grpc::InsecureChannelCredentials())));
    stub = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(coordinatorInfo, grpc::InsecureChannelCredentials())));

    // send a heartbeat to the coordinator, which registers your follower synchronizer as either a master or a slave

    // YOUR CODE HERE
    ClientContext context;
    
    ID id;
    id.set_id(synchID);
    
    csce662::ServerInfo serverinfo;

    grpc::Status status = stub->GetSynchronizer(&context, id, &serverinfo);

    // isMaster = serverinfo.ismaster();
    clusterSubdirectory = synchID<=3?"1":"2";
}

bool IsMaster() {
    ClientContext context;

    ID id;
    id.set_id(synchID);
    
    csce662::ServerInfo serverinfo;

    grpc::Status status = stub->IsMaster(&context, id, &serverinfo);

    return serverinfo.ismaster();
}

std::vector<int> GetOtherClusterSynchIDs() {
    ClientContext context;
    ID id;
    id.set_id(synchID);
    csce662::SynchIDs synchIDs;
    grpc::Status status = stub->GetOtherClusterSynchIDs(&context, id, &synchIDs);
    std::vector<int> other_synchIDs;
    for (int other_synchID : synchIDs.synchids() ){
        other_synchIDs.push_back(other_synchID);
    }

    return other_synchIDs;
}

bool file_contains_user(std::string filename, std::string user)
{
    std::vector<std::string> users;
    // check username is valid
    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + filename;
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
    users = get_lines_from_file(filename);
    for (int i = 0; i < users.size(); i++)
    {
        // std::cout<<"Checking if "<<user<<" = "<<users[i]<<std::endl;
        if (user == users[i])
        {
            // std::cout<<"found"<<std::endl;
            sem_close(fileSem);
            return true;
        }
    }
    // std::cout<<"not found"<<std::endl;
    sem_close(fileSem);
    return false;
}

std::vector<std::string> get_all_users_func(int synchID)
{
    // read all_users file master and client for correct serverID
    // std::string master_users_file = "./master"+std::to_string(synchID)+"/all_users";
    // std::string slave_users_file = "./slave"+std::to_string(synchID)+"/all_users";
    std::string clusterID = std::to_string(((synchID - 1) % 3) + 1);
    std::string master_users_file = "./cluster_" + clusterID + "/1/all_users.txt";
    std::string slave_users_file = "./cluster_" + clusterID + "/2/all_users.txt";
    // take longest list and package into AllUsers message
    std::vector<std::string> master_user_list = get_lines_from_file(master_users_file);
    std::vector<std::string> slave_user_list = get_lines_from_file(slave_users_file);

    // if (master_user_list.size() >= slave_user_list.size())
    //     return master_user_list;
    // else
    //     return slave_user_list;

    std::unordered_set<std::string> unique_set(master_user_list.begin(), master_user_list.end());
    unique_set.insert(slave_user_list.begin(), slave_user_list.end());

    // Convert the set back to a vector
    std::vector<std::string> combined_vector(unique_set.begin(), unique_set.end());

    return combined_vector;
}

std::vector<std::string> get_tl_or_fl(int synchID, int clientID, bool tl)
{
    // std::string master_fn = "./master"+std::to_string(synchID)+"/"+std::to_string(clientID);
    // std::string slave_fn = "./slave"+std::to_string(synchID)+"/" + std::to_string(clientID);
    std::string master_fn = "cluster_" + std::to_string(clusterID) + "/1/" + std::to_string(clientID);
    std::string slave_fn = "cluster_" + std::to_string(clusterID) + "/2/" + std::to_string(clientID);
    if (tl)
    {
        master_fn.append("_timeline.txt");
        slave_fn.append("_timeline.txt");
    }
    else
    {
        master_fn.append("_followers.txt");
        slave_fn.append("_followers.txt");
    }

    std::vector<std::string> m = get_lines_from_file(master_fn);
    std::vector<std::string> s = get_lines_from_file(slave_fn);

    if (m.size() >= s.size())
    {
        return m;
    }
    else
    {
        return s;
    }
}

std::vector<std::string> getFollowersOfUser(int ID)
{
    std::vector<std::string> followers;
    if (!clientInCluster(ID)) return followers;
    std::string clientID = std::to_string(ID);
    std::string filename = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + clientID + "_followers.txt";
    followers = get_lines_from_file(filename);
    return followers;
}

std::vector<std::string> getFollowingsOfUser(int ID)
{
    std::string filename = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + std::to_string(ID) + "_following.txt";    
    std::vector<std::string> followings = get_lines_from_file(filename);
    return followings;
}

bool clientInCluster(int ID) {
    std::string filename = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + std::to_string(ID) + "_following.txt";
    return std::filesystem::exists(filename);
}
