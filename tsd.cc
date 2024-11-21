/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <cstddef>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <ctime>
#include <csignal>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <unordered_map>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity); 

#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "sns.grpc.pb.h"
#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"
#include "client.h"


using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using csce662::Message;
using csce662::ListReply;
using csce662::Request;
using csce662::Reply;
using csce662::SNSService;
using csce662::CoordService;

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;


struct Client {
    std::string username;
    bool connected = true;
    int following_file_size = 0;
    std::vector<Client*> client_followers;
    std::vector<Client*> client_following;
    ServerReaderWriter<Message, Message>* stream = 0;
    // adding these two new variables below to monitor client heartbeats
    std::time_t last_heartbeat;
    bool missed_heartbeat = false;
    bool operator==(const Client& c1) const{
        return (username == c1.username);
    }
};

void checkHeartbeat();
std::time_t getTimeNow();

std::unique_ptr<csce662::CoordService::Stub> coordinator_stub_;

// coordinator rpcs
IReply Heartbeat(std::string clusterId, std::string serverId, std::string hostname, std::string port);

bool isMaster;
std::string clusterId;
std::string serverId;

//Vector that stores every client that has been created
/* std::vector<Client*> client_db; */

// using an unordered map to store clients rather than a vector as this allows for O(1) accessing and O(1) insertion
std::unordered_map<std::string, Client*> client_db;

// util function for checking if a client exists in the client_db and fetching it if it does
Client* getClient(std::string username){
    auto it = client_db.find(username);

    if (it != client_db.end()) {
        return client_db[username];
    } else {
        return NULL;
    }

}


class SNSServiceImpl final : public SNSService::Service {

    Status ClientHeartbeat(ServerContext* context, const Request* request, Reply* reply) override {

        std::string username = request->username();

        /* std::cout << "got a heartbeat from client: " << username << std::endl; */
        Client* c = getClient(username);
        if (c != NULL){
            c->last_heartbeat = getTimeNow();

        } else {
            std::cout << "client was not found, for some reason!\n";
            return Status::CANCELLED;
        }

        return Status::OK;
    }

    Status List(ServerContext* context, const Request* request, ListReply* list_reply) override {

        std::string filename = "./cluster_" + clusterId + "/" + serverId + "/all_users.txt";
        // std::vector<std::string> usernames;

        std::string semName = "/" + clusterId + "_" + serverId + "_" + "all_users.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

        std::ifstream file(filename);
        if (!file) {
            std::cerr << "Error: Unable to open file at " << filename << "\n";
            return Status::CANCELLED;
        }

        std::string username;
        while (std::getline(file, username)) {
            // usernames.push_back(username);
            list_reply->add_all_users(username);
        }

        file.close();
        sem_close(fileSem);

        return Status::OK;

        // // add all known clients to the all_users vector
        // for (const auto& pair : client_db){
        //     list_reply->add_all_users(pair.first);
        // }

        // std::string username = request->username();

        // // add all the followers of the client to the folowers vector
        // Client* c = getClient(username);
        // if (c != NULL){
        //     for (Client* x : c->client_followers){
        //         list_reply->add_followers(x->username);
        //     }

        // } else {
        //     return Status::CANCELLED;
        // }

        // return Status::OK;
    }

    Status Follow(ServerContext* context, const Request* request, Reply* reply) override {

        // std::string u1 = request->username();
        // std::string u2 = request->arguments(0);
        // Client* c1 = getClient(u1);
        // Client* c2 = getClient(u2);

        // if (c1 == nullptr || c2 == nullptr) { // either of the clients dont exist
        //     return Status(grpc::CANCELLED, "invalid username");
        // }

        // if (c1 == c2){ // if a client is asked to follow itself
        //     return Status(grpc::CANCELLED, "same client");
        // }



        // // check if the client to follow is already being followed
        // bool isAlreadyFollowing = std::find(c1->client_following.begin(), c1->client_following.end(), c2) != c1->client_following.end();

        // if (isAlreadyFollowing) {
        //     return Status(grpc::CANCELLED, "already following");
        // }

        // // add the clients to each other's relevant vector
        // c1->client_following.push_back(c2);
        // c2->client_followers.push_back(c1);

        // return Status::OK; 

        std::string u1 = request->username();
        std::string u2 = request->arguments(0);

        std::string filename = "./cluster_" + clusterId + "/" + serverId + "/" + u1 + "_following.txt";
            
        std::string semName = "/" + clusterId + "_" + serverId + "_" + u1 + "_following.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
        
        std::ofstream file(filename, std::ios::app);
        if (!file) {
            std::cerr << "Error: Unable to open or create file at " << filename << "\n";
            Status::CANCELLED;
        }
        file << u2 << "\n";
        file.close();
        
        sem_close(fileSem);

        return Status::OK;
    }

    Status UnFollow(ServerContext* context, const Request* request, Reply* reply) override {

        std::string username = request->username();
        // using a multimap to fetch the metadata out of the client's servercontext so we can check to see if a SIGINT was issued on the client's timeline
        const std::multimap<grpc::string_ref, grpc::string_ref>& metadata = context->client_metadata();

        auto it = metadata.find("terminated");
        if (it != metadata.end()) {
            std::string customValue(it->second.data(), it->second.length());

            std::string termStatus = customValue; // checking the value of the key "terminated" from the metadata in servercontext
            if (termStatus == "true"){

                Client* c = getClient(username);
                if (c != NULL){ // if the client exists, change its connection status and set its stream to null
                    c->last_heartbeat = getTimeNow();
                    c->connected = false;
                    c->stream = nullptr;
                }
                // DO NOT CONTINUE WITH UNFOLLOW AFTER THIS
                // Terminate here as this was not an actual unfollow request and just a makeshift way to handle SIGINTs on the client side
                return Status::OK;
            }

        }


        std::string u1 = request->username();
        std::string u2 = request->arguments(0);
        Client* c1 = getClient(u1);
        Client* c2 = getClient(u2);


        if (c1 == nullptr || c2 == nullptr) {
            return Status(grpc::CANCELLED, "invalid username");
        }

        if (c1 == c2){
            return Status(grpc::CANCELLED, "same client");
        }


        // Find and erase c2 from c1's following
        auto it1 = std::find(c1->client_following.begin(), c1->client_following.end(), c2);
        if (it1 != c1->client_following.end()) {
            c1->client_following.erase(it1);
        } else {
            return Status(grpc::CANCELLED, "not following");
        }

        // if it gets here, it means it was following the other client
        // Find and erase c1 from c2's followers
        auto it2 = std::find(c2->client_followers.begin(), c2->client_followers.end(), c1);
        if (it2 != c2->client_followers.end()) {
            c2->client_followers.erase(it2);
        }

        return Status::OK;
    }

    // RPC Login
    Status Login(ServerContext* context, const Request* request, Reply* reply) override {

        std::string username = request->username();

        Client* c = getClient(username);
        // if c exists 
        if (c != NULL){
            //  if an instance of the user is already active
            if (c->connected){
                c->missed_heartbeat = false;
                return Status::CANCELLED;
            } else { // this means the user was previously active, but inactive until just now
                c->connected = true;
                c->last_heartbeat = getTimeNow();
                c->missed_heartbeat = false;
                return Status::OK;
            }
        } else {
            // create a new client as this is a first time request from a new client
            Client* newc = new Client();
            newc->username = username;
            newc->connected = true;
            newc->last_heartbeat = getTimeNow();
            newc->missed_heartbeat = false;
            client_db[username] = newc;

            std::string filename = "./cluster_" + clusterId + "/" + serverId + "/all_users.txt";
            
            std::string semName = "/" + clusterId + "_" + serverId + "_" + "all_users.txt";
            sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
            
            std::ofstream file(filename, std::ios::app);
            if (!file) {
                std::cerr << "Error: Unable to open or create file at " << filename << "\n";
                Status::CANCELLED;
            }
            file << username << "\n";
            file.close();
            
            sem_close(fileSem);


            std::string filename2 = "./cluster_" + clusterId + "/" + serverId + "/" + username + "_followers.txt";
            std::ofstream file2(filename2, std::ios::app);
            if (!file2.is_open()) {
                std::cerr << "Error: Unable to create or open the file: " << filename2 << std::endl;
                return Status::CANCELLED;
            }
            file2.close();

            std::string filename3 = "./cluster_" + clusterId + "/" + serverId + "/" + username + "_following.txt";
            std::ofstream file3(filename3, std::ios::app);
            if (!file3.is_open()) {
                std::cerr << "Error: Unable to create or open the file: " << filename3 << std::endl;
                return Status::CANCELLED;
            }
            file3.close();
        }

        return Status::OK;
    }

    const int MAX_MESSAGES = 20;

    Status Timeline(ServerContext* context, ServerReaderWriter<Message, Message>* stream) override {

        // Initialize variables important for persisting timelines on the disk
        Message m;
        Client* c;
        std::string u;
        std::vector<std::string> latestMessages;
        std::vector<std::string> allMessages;
        bool firstTimelineStream = true;


        // multimap to fetch metadata from the servercontext which contains the username of the current client
        // this helps to Initialize the stream for this client as this is first contact
        const std::multimap<grpc::string_ref, grpc::string_ref>& metadata = context->client_metadata();

        auto it = metadata.find("username");
        if (it != metadata.end()) {
            std::string customValue(it->second.data(), it->second.length());

            // customValue is the username from the metadata received from the client
            u = customValue;
            c = getClient(u);
            c->stream = stream; // set the client's stream to be the current stream
        }

        // if this is the first time the client is logging back 
        if (firstTimelineStream && c != nullptr) {
            // Read latest 20 messages from following file
            std::ifstream followingFile(u + "_following.txt");
            if (followingFile.is_open()) {
                std::string line;
                while (std::getline(followingFile, line)) {
                    allMessages.push_back(line);
                }

                // Determine the starting index for retrieving latest messages
                int startIndex = std::max(0, static_cast<int>(allMessages.size()) - MAX_MESSAGES);

                // Retrieve the latest messages
                for (int i = startIndex; i < allMessages.size(); ++i) {
                    latestMessages.push_back(allMessages[i]);
                }
                std::reverse(latestMessages.begin(), latestMessages.end()); // reversing the vector to match the assignment description
                followingFile.close();
            }

            // Send latest 20 messages to client via the grpc stream
            for (const std::string& msg : latestMessages) {
                Message latestMessage;
                latestMessage.set_msg(msg + "\n");
                stream->Write(latestMessage);
            }
            firstTimelineStream = false;
        }


        while (stream->Read(&m)) { // while there are messages being sent by the client over the stream

            if (c != nullptr) {

                // Convert timestamp to string
                std::time_t timestamp_seconds = m.timestamp().seconds();
                std::tm* timestamp_tm = std::gmtime(&timestamp_seconds);

                char time_str[50]; // Make sure the buffer is large enough
                std::strftime(time_str, sizeof(time_str), "%a %b %d %T %Y", timestamp_tm);

                std::string ffo = u + '(' + time_str + ')' + " >> " + m.msg();

                // Append to user's timeline file
                std::ofstream userFile(u + ".txt", std::ios_base::app);
                if (userFile.is_open()) {
                    userFile.seekp(0, std::ios_base::beg);
                    userFile << ffo;
                    userFile.close();
                }


                // Send the new message to all followers for their timeline
                for (Client* follower : c->client_followers) {
                    if (follower->stream != nullptr) {
                        Message followerMessage;
                        followerMessage.set_msg(ffo);

                        if (follower->stream != nullptr) {
                            follower->stream->Write(followerMessage);
                        } 

                    } 
                }

                // Append to  all the followers' following file
                for (Client* follower : c->client_followers) {
                    std::ofstream followerFile(follower->username + "_following.txt", std::ios_base::app);
                    if (followerFile.is_open()) {
                        followerFile.seekp(0, std::ios_base::beg);
                        followerFile << ffo;
                        followerFile.close();
                    }
                }
            } 

        }

        return Status::OK;
    }

};

// function that sends a heartbeat to the coordinator
IReply Heartbeat(std::string clusterId, std::string serverId, std::string hostname, std::string port, bool isHeartbeat) {

    IReply ire;

    // creating arguments and utils to make the gRPC
    ClientContext context;
    csce662::ServerInfo serverinfo;
    csce662::Confirmation confirmation;


    if (isHeartbeat){
        context.AddMetadata("heartbeat", "Hello"); // adding the server's clusterId in the metadata so the coordinator can know
    }

    context.AddMetadata("clusterid", clusterId); // adding the server's clusterId in the metadata so the coordinator can know

    int intServerId = std::stoi(serverId);

    serverinfo.set_serverid(intServerId);
    serverinfo.set_hostname(hostname);
    serverinfo.set_port(port);

    grpc::Status status = coordinator_stub_->Heartbeat(&context, serverinfo, &confirmation);
    
    isMaster = confirmation.status();

    // std::cout << "Server in cluster " << clusterId << " server id " << serverId << " is master " << isMaster << std::endl;
    
    if (status.ok()){
        ire.grpc_status = status;
    }else { // professor said in class that since the servers cannot be run without a coordinator, you should exit

        ire.grpc_status = status;
        std::cout << "coordinator not found! exiting now...\n";
    }

    return ire;
}

// function that runs inside a detached thread that calls the heartbeat function
void sendHeartbeat(std::string clusterId, std::string serverId, std::string hostname, std::string port) {
    while (true){

        sleep(3);

        IReply reply = Heartbeat(clusterId, serverId, "localhost", port, true);
        if (!reply.grpc_status.ok()){
            exit(1);
        }
    }

}

void RunServer(std::string clusterId, std::string serverId, std::string coordinatorIP, std::string coordinatorPort, std::string port_no) {
    std::string server_address = "0.0.0.0:"+port_no;
    SNSServiceImpl service;

    // running the heartbeat function to monitor heartbeats from the clients
    std::thread hb(checkHeartbeat);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    log(INFO, "Server listening on "+server_address);


    // FROM WHAT I UNDERSTAND, THIS IS THE BEST PLACE TO REGISTER WITH THE COORDINATOR

    // need to first create a stub to communicate with the coordinator to get the info of the server to connect to
    std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
    grpc::ChannelArguments channel_args;
    std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
            coordinator_address, grpc::InsecureChannelCredentials(), channel_args);

    // Instantiate the coordinator stub
    coordinator_stub_ = csce662::CoordService::NewStub(channel);
    IReply reply = Heartbeat(clusterId, serverId, "localhost", port_no, false);
    if (!reply.grpc_status.ok()){
        // EXITING AS COORDINATOR WAS NOT REACHABLE
        exit(0);
    }

    // running a thread to periodically send a heartbeat to the coordinator
    std::thread myhb(sendHeartbeat, clusterId, serverId, "localhost", port_no);

    myhb.detach();


    server->Wait();
}


void checkHeartbeat(){
    while(true){
        //check clients for heartbeat > 3s

        for (const auto& pair : client_db){
            if(difftime(getTimeNow(),pair.second->last_heartbeat) > 3){
                std::cout << "missed heartbeat from client with id " << pair.first << std::endl;
                if(!pair.second->missed_heartbeat){
                    Client* current = getClient(pair.first);
                    if (current != NULL){
                        std::cout << "setting the client's values in the DB to show that it is down!\n";
                        current->connected = false;
                        current->stream = nullptr;
                        current->missed_heartbeat = true;
                        current->last_heartbeat = getTimeNow();
                    } else{
                        std::cout << "SUDDENLY, THE CLIENT CANNOT BE FOUND?!\n";
                    }
                }
            }
        }

        sleep(3);
    }
}

int main(int argc, char** argv) {

    clusterId = "1";
    // std::string serverId = "1";
    serverId = "1";
    std::string coordinatorIP = "localhost";
    std::string coordinatorPort = "9090";
    std::string port = "1000";

    int opt = 0;
    while ((opt = getopt(argc, argv, "c:s:h:k:p:")) != -1){
        switch(opt) {
            case 'c':
                clusterId = optarg;break;
            case 's':
                serverId = optarg;break;
            case 'h':
                coordinatorIP = optarg;break;
            case 'k':
                coordinatorPort = optarg;break;
            case 'p':
                port = optarg;break;
            default:
                std::cout << "Invalid Command Line Argument\n";
        }
    }


    std::string log_file_name = std::string("server-") + port;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Server starting...");

    /* RunServer(port); */
    // changing this call so i can pass other auxilliary variables to be able to communicate with the server
    RunServer(clusterId, serverId, coordinatorIP, coordinatorPort, port);

    return 0;
}

std::time_t getTimeNow(){
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}
