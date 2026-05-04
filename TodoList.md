# TodoList

## Project description

The project is a dedicated game server for godot 4.6+ in C++ using a gdextension, godot-cpp, and enTT.

Read the readme for more info on how to run the project.

The goal of this server is to allow me to create various games without touching to the server code or engine that much.

You can see a more advanced project example in `./GodotPPCorrection/`

## ToDos

### Quick fixes

1. Sync clocks (mean of 50% low RTTs / 2)
2. slew clock if too fast (modify delta time so clock speeds up a bit (+1ms max))

## State machine and protocol

We use a custom UDP protocol but here the server is dumb. We need to add a state machine so the server can handle connections better

We would have a Not Connected state where we send a HELO packet every 100ms to try to connect and timeout after 1s
If we get a response we go to the connecting state where we send a HSK packet every 100ms and timeout after 1s
Then we can go in the connected state where we send DATA about the game
If no data after 100ms we get in the spurrious state where we still send DATA but also pings
If pong we get back to connected if timeout we get disconnected

## World Serializer

Here in the game the map is empty and the server creates the player characters. I want to be able to level design so i 
need to serialize info about the game scene instead of just replicating player positions.

Idk what's the best way to give our map to the server (or at the very least the colisions) so you pan propose anything 
that's suitable

## Prediction and correction

Here's the big challenge : I want to implement prediction and correction on the client side. The server will be the 
authority but the client will predict the movement of the player and correct it when it receives data from the server.

For the way of implementing it, check the notes i taken in `./predictionNotes.md`
