
/*

 Jog Dial for ChiliPeppr and bCNC

 This code is meant to allow button, jog, and shuttle events flow
 via a websocket connection
 
 For Chilipeppr it connects to the Serial Port JSON server (SPJS) typically used 
 with ChiliPeppr driven CNC machines.

 For bCNC it connects to the HTTP server implemented in bCNC.  This server
 is typically used for a simple HTML pendant-style control but is also
 capable of sending gcode directly to GRBL
 Note that at the time of this writing, bCNC is specific to GRBL only.  bCNC does not work with TinyG.

 Interface to Shuttle Contour Xpress based on "Contour ShuttlePro
 v2 interface" by Eric Messick.

*/

#include "shuttle.h"

#if GPIO_SUPPORT
#include "led_control.h"
#include "raspi_switches.h"
#include <wiringPi.h>
#endif

#include "websocket.h"

#define CNC_HOST      "localhost"         // Hostname where SPJS or bCNC is running
#define CNC_PORT      "8989"              // Port for SPJS or bCNC.  Typically 8989 for Chillipeppr and 8080 for bCNC
#define DEVICE_PATH   "/dev/ttyACM0"      // Path for SPJS to connect to GRBL or TinyG.  Not used for bCNC
#define TINYG         0                   // set to 1 if you are using a TinyG
#define BCNC          0                   // set to 1 if you are using bCNC instead of Chilipeppr
#define CYCLE_TIME_MICROSECONDS 100000    // time of each main loop iteration
#define MAX_FEED_RATE 1500.0              // (unit per minute - initially tested with millimeters)
#define OVERSHOOT     1.06                // amount of overshoot for shuttle wheel

// Each press of the increment button will toggle through 4 speed / distance
// increments. Use the defines below to adjust the distance moved by 
// each increment of the jog dial.
// These line up with MOTION_SPEED_1, MOTION_SPEED_2, etc.
#define INCREMENT1 0.001
#define INCREMENT2 0.01
#define INCREMENT3 0.1
#define INCREMENT4 1.0

typedef struct input_event EV;
unsigned short jogvalue = 0xffff;
int            shuttlevalue = 0xffff;
struct timeval last_shuttle;
int            need_synthetic_shuttle;

#if GPIO_SUPPORT
LED_STATES    led_states;
SWITCH_STATES raspi_switches;
#endif
short int     cnc_connected      = 0;
short int     reconnect_requested      = 0;
short int     shuttle_device_connected = 0;
ACTIVE_AXIS   active_axis              = X_AXIS_ACTIVE;
ACTIVE_SPEED  active_speed             = MOTION_SPEED_4;
Queue         cmd_queue;
char          lastcmd[MAX_CMD_LENGTH];
short int     continuously_send_last_command;


// A utility procedure to send a command generated by one of the 
// switch interrupt service routines below.
void generic_switch_command( const char *sw_name, char cmdchar ) {
    char cmd[MAX_CMD_LENGTH];
    fprintf(stderr, "%s detected\n", sw_name);
    if (!BCNC) {
        snprintf( cmd, MAX_CMD_LENGTH, "send %s %c\n", DEVICE_PATH, cmdchar );
    } else {
        snprintf( cmd, MAX_CMD_LENGTH, "/send %c", cmdchar );
    }
    cmd_queue.clear(&cmd_queue);  // clear all other commands
    continuously_send_last_command = 0;
    cmd[MAX_CMD_LENGTH-1] = '\0';
    cmd_queue.push( &cmd_queue, cmd );
}

#if GPIO_SUPPORT
// A procedure to look at the raspi switch states and if
// one of them is pressed, queue up the appropriate command
void process_raspi_switches( SWITCH_STATES *sw ) 
{
    // check if any switch that needs a command is depressed,
    // then check and see if it is different from its previous state.
    if (!sw->feed_hold || !sw->resume || !sw->reset) {
        if (!sw->feed_hold && (sw->feed_hold != sw->prev_feed_hold)) {
            generic_switch_command( "FEED_HOLD", '!');
        }
        if (!sw->resume && (sw->resume != sw->prev_resume)) {
            generic_switch_command( "RESUME", '~');
        }
        if (!sw->reset && (sw->reset != sw->prev_reset)) {
            generic_switch_command( "RESET", 24);
        }
    }

    // Now check other switches that don't spawn commands
    if (!sw->reconnect_requested && (sw->reconnect_requested != sw->prev_reconnect_requested)) {
        fprintf(stderr, "RECONNECT detected\n");
        reconnect_requested = 1;
    }
}
#endif

// A helper procedure to return the character used for each axis
// and the current speed/increment level.
void get_axis_and_speed( char* axis, float* speed ) {
    switch (active_axis) {
        case X_AXIS_ACTIVE: *axis = 'X'; break;
        case Y_AXIS_ACTIVE: *axis = 'Y'; break;
        case Z_AXIS_ACTIVE: *axis = 'Z'; break;
        case A_AXIS_ACTIVE: *axis = 'A'; break;
        default:            *axis = 'X'; break;
    }
    switch (active_speed) {
        case MOTION_SPEED_1: *speed = INCREMENT1; break;
        case MOTION_SPEED_2: *speed = INCREMENT2; break;
        case MOTION_SPEED_3: *speed = INCREMENT3; break;
        case MOTION_SPEED_4: *speed = INCREMENT4; break;
        default:             *speed = INCREMENT2; break;
    }
}


// Main event procedure whenever a button is pressed.
void key(unsigned short code, unsigned int value)
{
    char axis;
    float speed;
    char cmd[MAX_CMD_LENGTH];
    short bcast_axis  = 0;
    short bcast_speed = 0;

    // Only work on value == 1, which is the button down event
    if (value == 1) {
        switch (code) {
            case X_AXIS_BUTTON: active_axis = X_AXIS_ACTIVE; bcast_axis = 1; break;
            case Y_AXIS_BUTTON: active_axis = Y_AXIS_ACTIVE; bcast_axis = 1; break;
            case Z_AXIS_BUTTON: active_axis = Z_AXIS_ACTIVE; bcast_axis = 1; break;
            case A_AXIS_BUTTON: active_axis = A_AXIS_ACTIVE; bcast_axis = 1; break;
            case INCREMENT_BUTTON: {
                active_speed = (active_speed + 1) % NUM_MOTION_SPEEDS;
                bcast_speed = 1;
                break;
            }
            default:
                fprintf(stderr, "key(%d, %d) out of range\n", code, value);
                break;
        }
        get_axis_and_speed( &axis, &speed );

        // If we need to broadcast the active axis or speed, send that out.
        if (!BCNC) { // Only perform the following for Chilipeppr
            if (bcast_axis) {
                snprintf( cmd, MAX_CMD_LENGTH, "broadcast {\"id\":\"shuttlexpress\", \"action\":\"%c\"}\n", tolower(axis) );
            } else if (bcast_speed) {
                snprintf( cmd, MAX_CMD_LENGTH, "broadcast {\"id\":\"shuttlexpress\", \"action\":\"%.3fmm\"}\n", speed );
            }
            cmd[MAX_CMD_LENGTH-1] = '\0';
            cmd_queue.push( &cmd_queue, cmd );
            fprintf(stderr, "%s", cmd);
        }
    }
}


// Main event procedure whenever shuttle wheel is turned.
void shuttle(int value)
{
    char cmd[MAX_CMD_LENGTH];
    char axis;
    float speed;
    float distance;
    int direction;

    if (value < -7 || value > 7) {
        fprintf(stderr, "shuttle(%d) out of range\n", value);
    } else {
        fprintf(stderr, "Received shuttle command for value %d ???\n", value);
        direction = (value >= 0) ? 1 : -1;
        gettimeofday(&last_shuttle, 0);
        need_synthetic_shuttle = value != 0;
        if( value != shuttlevalue ) {
            shuttlevalue = value;
        }

        // if we got a shuttle of zero (0), this is our indication to 
        // stop streaming commands. Since there is a bug and sometimes
        // the shuttle doesn't send the event for zero, we actually 
        // stop on 0 or 1.
        cmd_queue.clear(&cmd_queue);  // when we are shuttling, never queue commands
        if ((value == 0) || (value == 1) || (value == -1)) {
            continuously_send_last_command = 0;

            // Sending the wipe (%) command to GRBL doesn't work, but this
            // should help for TinyG.  In reality, for TinyG we should really send
            // a feed hold, then a wipe, then a resume.  Hopefully someone can 
            // implement and test this on a TinyG.  TODO
            if (TINYG) {
                snprintf( cmd, MAX_CMD_LENGTH, "send %s !%%\n", DEVICE_PATH );
                cmd_queue.push( &cmd_queue, cmd );
            }

        } else {
            continuously_send_last_command = 1;
            get_axis_and_speed( &axis, &speed );

            // We calculate how far we want to move based on our current increment
            // setting and our current cycle time.  That way, we know roughly how
            // far the machine will move and can have the next command queued up
            // just before it starts to decelerate, which is why we have the
            // overshoot factor.
            speed = speed * direction * value * (MAX_FEED_RATE / (7.0*INCREMENT4));
            distance = (speed/60.0) * (CYCLE_TIME_MICROSECONDS * OVERSHOOT / 1000000.0) * direction;
            if (!BCNC) {
                snprintf( cmd, MAX_CMD_LENGTH, "send %s G91 G1 F%.3f %c%.3f\nG90\n", DEVICE_PATH, speed, axis, distance );
            } else {
                snprintf( cmd, MAX_CMD_LENGTH, "http://%s:%s/send?gcode=G91G1F%.3f%c%.3f%%0DG90", CNC_HOST, CNC_PORT, speed, axis, distance );
            }
            strncpy( lastcmd, cmd, MAX_CMD_LENGTH );
            cmd[MAX_CMD_LENGTH-1] = lastcmd[MAX_CMD_LENGTH-1] = '\0';
            cmd_queue.push( &cmd_queue, cmd );
        }
    }
}

// Due to a bug (?) in the way Linux HID handles the ShuttlePro, the
// center position is not reported for the shuttle wheel.  Instead,
// a jog event is generated immediately when it returns.  We check to
// see if the time since the last shuttle was more than a few ms ago
// and generate a shuttle of 0 if so.
//
// Note, this fails if jogvalue happens to be 0, as we don't see that
// event either!
void jog(unsigned int value)
{
    int direction;
    struct timeval now;
    struct timeval delta;
    char axis;
    float distance;
    char cmd[MAX_CMD_LENGTH];

    // I think the reason we want to skip the very first jog is
    // because we can't calculate direction until we get 2 jog
    // events. --FG
    if ((jogvalue != 0xffff) && (jogvalue != value)) {
        direction = ((value - jogvalue) & 0x80) ? -1 : 1;
        get_axis_and_speed( &axis, &distance );
        distance *= direction;
        if (!BCNC) {
            snprintf( cmd, MAX_CMD_LENGTH, "send %s G91 G0 %c%.3f\nG90\n", DEVICE_PATH, axis, distance );
        } else {
            snprintf( cmd, MAX_CMD_LENGTH, "http://%s:%s/send?gcode=G91G0%c%.3f%%0DG90", CNC_HOST, CNC_PORT, axis, distance );
        }
        strncpy( lastcmd, cmd, MAX_CMD_LENGTH );
        cmd[MAX_CMD_LENGTH-1] = lastcmd[MAX_CMD_LENGTH-1] = '\0';
	cmd_queue.push( &cmd_queue, cmd );
    }
    jogvalue = value;

    // We should generate a synthetic event for the shuttle going
    // to the home position if we have not seen one recently
    if (need_synthetic_shuttle) {
        gettimeofday( &now, 0 );
        timersub( &now, &last_shuttle, &delta );

        if (delta.tv_sec >= 1 || delta.tv_usec >= 5000) {
            shuttle(0);
            need_synthetic_shuttle = 0;
        }
    }

    if (jogvalue != 0xffff) {
        value = value & 0xff;
        direction = ((value - jogvalue) & 0x80) ? -1 : 1;
        while (jogvalue != value) {
            // driver fails to send an event when jogvalue == 0
            jogvalue = (jogvalue + direction) & 0xff;
        }
    }
    jogvalue = value;
}

// Handler for jog and shuttle events, which calls the appropriate
// function for each.
void jogshuttle(unsigned short code, unsigned int value)
{
    switch (code) {
        case EVENT_CODE_JOG:
            jog(value);
            break;
        case EVENT_CODE_SHUTTLE:
            shuttle(value);
            break;
        default:
            fprintf(stderr, "jogshuttle(%d, %d) invalid code\n", code, value);
            break;
    }
}


// Toplevel event handler
void handle_event(EV ev)
{
    switch (ev.type) {
        case EVENT_TYPE_DONE:
        case EVENT_TYPE_ACTIVE_KEY:
            break;
        case EVENT_TYPE_KEY:
            key(ev.code, ev.value);
            break;
        case EVENT_TYPE_JOGSHUTTLE:
            jogshuttle(ev.code, ev.value);
            break;
        default:
            fprintf(stderr, "handle_event() invalid type code\n");
            break;
    }
}

// A helper procedure to reset the program and cause the connection
// to the websocket and to the shuttle device to be re-initialized.
void reset_connections() {
    fprintf(stderr, "============ Reinitializing connections\n");
    cmd_queue.clear( &cmd_queue );
    continuously_send_last_command = 0;
    shuttle_device_connected = 0;
    cnc_connected = 0;
#if GPIO_SUPPORT
    update_led_states( &led_states, shuttle_device_connected, cnc_connected, active_axis, active_speed );
    drive_leds( &led_states );
#endif
}


int
main(int argc, char **argv)
{
    EV ev;
    int nread;
    char *dev_name;
    int fd, num_cmds_in_queue, num_cmds_sent;
    struct timeval time_start;
    struct timeval time_end;
    struct timeval time_taken;
    struct timeval time_to_sleep;
    struct timeval cycle_time_us;
    struct timeval select_timeout;
    unsigned int sleep_us;
    fd_set set;
    char host[256];
    char port[16];

    select_timeout.tv_sec = 0;
    select_timeout.tv_usec = 0;

    cycle_time_us.tv_sec = 0;
    cycle_time_us.tv_usec = CYCLE_TIME_MICROSECONDS;

    if (argc != 2) {
        fprintf(stderr, "usage: shuttlecp <device>\n" );
        exit(1);
    }

    dev_name = argv[1];

    // initialize LEDs and switches
#if GPIO_SUPPORT
    wiringPiSetup(); 
    initialize_led_states( &led_states );
    initialize_raspi_switch_states( &raspi_switches );
    drive_leds( &led_states );
#endif

    cmd_queue = createQueue();
    fd = -1;
    shuttle_device_connected = 0;

    while (1) {

        // Skip initialisation of websocket if bCNC is being used
        if (!BCNC) {
            // initialize - open websocket and open device
            snprintf(host, sizeof(host), CNC_HOST);
            snprintf(port, sizeof(port), CNC_PORT);
            fprintf(stderr, "Attempting connection to %s:%s\n", host, port);
            while ( websocket_init( host, port ) ) {
                fprintf(stderr, "Attempting connection to %s:%s\n", host, port);
                usleep(1000000);
            }
            cnc_connected = 1;
            reconnect_requested = 0;
            fprintf(stderr, "Websocket connected.\n");
        }
        else {
            cnc_connected = 1;
            reconnect_requested = 0;
            fprintf(stderr, "HTTP used for bCNC.\n");            
        }
#if GPIO_SUPPORT
        update_led_states( &led_states, shuttle_device_connected, cnc_connected, active_axis, active_speed );
        drive_leds( &led_states );
#endif

        // Open the connection to the device - loop until
        // we connect.
        while (!shuttle_device_connected) {
            fd = open(dev_name, O_RDONLY);
            if (fd < 0) {
                perror(dev_name);
                sleep(1);
                continue;
            }

            // Flag it as exclusive access
            if(ioctl( fd, EVIOCGRAB, 1 ) < 0) {
                perror( "evgrab ioctl" );
                sleep(1);
                continue;
            }

            // if we get to here, we're connected
            shuttle_device_connected = 1;
            fprintf(stderr, "Shuttle device connected.\n");
        }

        // The main loop we operate in
        while (1) {

            // if we have lost connection to the websocket, or if we have
            // a request to reconnect everything, then break
            // the loop so we can reinitialize everything.
            if (!cnc_connected || reconnect_requested) {
                reset_connections();
                break;
            }

            gettimeofday( &time_start, 0 );

            // We are going to just select on the FD to see if a read
            // would produce any data.  If not, we skip the read.
            FD_ZERO(&set);
            FD_SET(fd, &set);

            while (select(fd+1, &set, NULL, NULL, &select_timeout)) {
                // read jog controller events
                nread = read(fd, &ev, sizeof(ev));
                if (nread == sizeof(ev)) {
                    handle_event(ev);
                } else {
                    if (nread < 0) {
                        perror("read event");
                        reconnect_requested = 1;
                        shuttle_device_connected = 0;
                        break;
                    } else {
                        fprintf(stderr, "short read: %d\n", nread);
                        reconnect_requested = 1;
                        shuttle_device_connected = 0;
                        break;
                    }
                }
            }

            // read raspberry pi buttons/switches
#if GPIO_SUPPORT
            read_raspi_switches( &raspi_switches );
            process_raspi_switches( &raspi_switches );
#endif

            // send all queued commands
            if (cnc_connected) {

                if (!BCNC) {
                    num_cmds_in_queue = cmd_queue.size;
                    num_cmds_sent = websocket_send_cmds( &cmd_queue );
                    if (num_cmds_sent != num_cmds_in_queue) {
                        cnc_connected = 0;
                    }
                } else {
                    http_send_cmds( &cmd_queue);
                }

                // If we should be continuously sending a cmd, enqueue it here
                if ( continuously_send_last_command && cnc_connected ) {
                    cmd_queue.push( &cmd_queue, lastcmd );
                }
            }

#if GPIO_SUPPORT
            // update LEDs
            update_led_states( &led_states, shuttle_device_connected, cnc_connected, active_axis, active_speed );
            drive_leds( &led_states );
#endif

            // sleep until next cycle
            gettimeofday( &time_end, 0 );
            timersub( &time_end, &time_start, &time_taken );
            timersub( &cycle_time_us, &time_taken, &time_to_sleep );
            if (time_to_sleep.tv_sec < 0) {
                sleep_us = 0;
            } else {
                sleep_us = time_to_sleep.tv_usec;
                usleep( sleep_us );
            }
        }

        close(fd);
        sleep(1);
    }
}
