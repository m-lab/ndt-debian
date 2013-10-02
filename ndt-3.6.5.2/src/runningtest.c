/**
 * This file contains functions that help get/set the currently
 * running test
 *
 *  Created on: Sep 28, 2011
 *      Author: kkumar
 */
#include "ndtptestconstants.h"
#include "runningtest.h"

static int currentTest = TEST_NONE;
static enum Tx_DIRECTION currentDirection = NONE;
static char *senddirnstr;
static char *recvdirnstr;

/**
 * array defining possible events pertaining to process status
 */
static char *_procstatusarray[] = { "unknown", "started", "completed" };

/** array defining various "processes" like a web100srv process,
 * or a client connection
 */
static char *_proctypesarray[] = { "process", "connect" };

/**
 * Get ID of currently running test
 * @return integer current running test Id
 * */
int getCurrentTest() {
  return currentTest;
}

/** Set the id of the currently running test.
 * @param testId Id of the currently running test
 */

void setCurrentTest(int testId) {
  currentTest = testId;
}

/**
 * Get the current direction of the test.
 * @return integer direction corresponding to an enumerator
 * */
int getCurrentDirn() {
  return currentDirection;
}

/** Set the test directions for the current process.
 * For example, for server process,
 * the current-test-direction = S->C (send direction)
 * , and the other/reverse
 * direction is C->S (i.e the receive direction)
 * This method has to be invoked by every process
 * that decides to have protocol logging.
 * An example direction: client->server or vice
 * versa.
 * @param directionarg direction of the currently running process
 */

void setCurrentDirn(enum Tx_DIRECTION directionarg) {
  char currenttestdirn[TEST_DIRN_DESC_SIZE];
  char othertestdirn[TEST_DIRN_DESC_SIZE];
  currentDirection = directionarg;
  switch (currentDirection) {
    case S_C:
      senddirnstr = get_testdirectiondesc(currentDirection, currenttestdirn);
      recvdirnstr = get_testdirectiondesc(C_S, othertestdirn);
      break;
    case C_S:
      senddirnstr = get_testdirectiondesc(currentDirection, currenttestdirn);
      recvdirnstr = get_testdirectiondesc(S_C, othertestdirn);
      break;
    case NO_DIR:
    default:
      senddirnstr = get_testdirectiondesc(NO_DIR, currenttestdirn);
      recvdirnstr = get_testdirectiondesc(NO_DIR, othertestdirn);
      break;
  }
}

/**
 * Get a description of the currently running test
 * @return descriptive name for the currently running test
 */
char *get_currenttestdesc() {
  enum TEST_ID currenttestId = NONE;
  char currenttestdesc[TEST_NAME_DESC_SIZE];
  switch (getCurrentTest()) {
    case TEST_MID:
      currenttestId = MIDDLEBOX;
      break;
    case TEST_C2S:
      currenttestId = C2S;
      break;
    case TEST_S2C:
      currenttestId = S2C;
      break;
    case TEST_SFW:
      currenttestId = SFW;
      break;
    case TEST_META:
      currenttestId = META;
      break;
    case TEST_NONE:
    default:
      currenttestId = NONE;
      break;
  }
  return get_testnamedesc(currenttestId, currenttestdesc);
}

/**
 * get the current (send) direction. For example,
 *  if this is the server process, then local direction = S->C
 *  @return char* descriptive text of the current local direction
 *  */
char *get_currentdirndesc() {
  return senddirnstr;
}

/**
 * get the current reverse(recv) direction. For example,
 *  if this is the server process, then local direction = C->S
 *  @return char* descriptive text of the current reverse direction
 *  */
char *get_otherdirndesc() {
  return recvdirnstr;
}

/**
 * get descriptive test for status of process
 *  @return char* descriptive text of the process status
 *  */
char *get_procstatusdesc(enum PROCESS_STATUS_INT procstatusarg,
                         char *sprocarg) {
  sprocarg = _procstatusarray[procstatusarg];
  // log_println(7,"--current process status = %s for %d\n", sprocarg,
  //  procstatusarg);
  return sprocarg;
}

/**
 * Get descriptive string for process name
 * @param procid Id of the test
 * @param *snamearg  storage for test name description
 * @return char*  Descriptive string for process (process, connect )
 * */
char *get_processtypedesc(enum PROCESS_TYPE_INT procidarg, char *snamearg) {
  snamearg = _proctypesarray[procidarg];
  // log_println(7,"--current process name = %s for %d\n", snamearg, procidarg);
  return snamearg;
}
