# 3DPrintedArcadeMachine
PACMAN

Lab 4: Interactive 3D Printed Devices

DEMO video: https://youtube.com/shorts/QaWJU6Ro54Q

What it is

A handheld, standalone Pac-Man arcade machine. The enclosure was designed in SolidWorks and 3D printed. It houses an ESP32, an OLED screen, a joystick, a push button, and three LEDs. The game runs entirely on the ESP32, so it only needs USB power and no computer.

The 3D-printed device is the interactive system: you steer Pac-Man with the joystick, press the button to start and exit, and the LEDs along the top react to what is happening in the game.

How to play
Joystick: steer Pac-Man through the maze.
Button: start the game from the home screen, and return to the home screen from the game over screen.
Eat all the pellets to win. Avoid the four ghosts.
Eat a large power pellet (the blinking beads in the corners), and the ghosts turn frightened for about 7 seconds. You can eat them for 200, 400, 800, then 1600 points.

AI USE: I used AI to modify pre-existing PACMAN code to be better suited for a 2.4-inch single-color screen. I worked with it to make new ghost characters and reduce some visuals to fit everything in the screen without it being too crowded.
