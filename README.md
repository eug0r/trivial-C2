# trivial-C2
## Overview
trivial-C2 is a lightweight Command and Control (C2) framework built in C, for research and practice. \
It implements a client-server architecture setup where agents register with a central server, receive tasks, and return results, all managed through a simple control client. \
The goal was to explore how such systems work at a lower level and to understand the typical challenges involved. For that reason, the project is written in C with minimal reliance on third-party libraries. \
The server exposes a simple 'REST' API over HTTPS for: \
- Agent registration (tracking connected clients)
- Tasking (issuing commands to selected agents)
- Result collection (receiving and storing task outputs) \
  
The HTTP-server is completely home-brew and minimal. Openssl is used for TLS, Sqlite for the database, and Jannson for handling JSON. The Clients, however, use Libcurl for HTTPS connection.

## Testing
- start the server using the build-and-run.sh script
- server listens to connection on port 4221, you may change it via HTTP_PORT_NO macro in server/http-server.c
- build and run agent(s) on a supported system using build-and-run.sh, or build it with just-build.sh and run it on a supported system.
- agent tries to connect to 127.0.0.1:4221, you may change it via the macros in agent/config.h
- remove the `#define DEBUG` line from agent/config.h or add `#undef DEBUG` right below it to disable all logging to stdout and stderr.
- start the control CLI using build-and-run.sh
- The server's TLS certificate is _self-signed_, and its _relative path_, under `certs/`, is hardcoded.
  - You may replace this and adjust the path as needed. For agents and control CLI it's defined as `SERVER_CERT_PATH` macro, in `agent*/src/config.h` and `control/src/main.c` respectively. For the server it's defined in `server/src/http-server.h`.
  - There's also a script under `certs/` for generating a new TLS certificate.
- a test script has been added in the server directory, for stress-testing the server, against parallel requests.

## Development
### Server:
compile and run: ./build-and-run.sh \
__replace the vcpkg directory path with your own.__ \
__to use the testing database, change `C2_DB_PATH` in src/handlers.h to testdatabase.db.__ \
If you wish to use your own libuuid installation remove it from vcpkg.json and replace its linking command in CMakeLists.txt.

#### API:
__post /agents:__
- expects json object
- `handle` string (given name) and `hostname` (victim's hostname extracted by the agent) must be present.
- returns an agent-uuid that must be kept by the agent to use it for querying for tasks \

__get /agents:__
- returns json array
- no `handle` query param: all agents
- `handle` query param: only agent(s) with that handle

__post /tasks:__
- expects json object
- `category` string, `agent_id` string, `options` object must be present
- returns task uuid
  get /tasks:
- returns json array
- current version only returns a single task object in the array
- expects an `id` query param which should be the agent's uuid.
- the returned task will be the earliest unfinished task in the queue.

__post /results:__
- expects json object
- requires `agent_id` and `task_id` strings and a results object (furthermore the results object needs a `type` string that should indicate whether the `data` string is plain-text or base64 encoded binary data. this won't be validated by the server.)

__get /results:__
- returns json array
- no `agent-id` or `task-id` query params: all results
- `agent-id` or `task-id` query params: respective results

