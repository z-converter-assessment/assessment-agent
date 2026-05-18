/**
 * @file service.h
 * @brief Windows Service Control Manager dispatcher.
 *
 * The agent runs in one of two modes:
 *   - Service mode (default when launched by SCM via `sc.exe start`):
 *     StartServiceCtrlDispatcher → ServiceMain → RegisterServiceCtrlHandlerEx
 *     → collection loop until SCM sends SERVICE_CONTROL_STOP.
 *
 *   - Console mode (`assessment-agent.exe --console`): bypass SCM, run the
 *     same collection loop with a Ctrl+C handler. Used for local debugging
 *     and during install.ps1 smoke tests.
 *
 * SERVICE_NAME 은 install.ps1 의 `sc.exe create` 명칭과 정확히 일치해야 함.
 */

#ifndef ASSESSMENT_AGENT_SERVICE_H
#define ASSESSMENT_AGENT_SERVICE_H

#define ASSESSMENT_AGENT_SERVICE_NAME L"assessment-agent"

/**
 * @brief Hand off to the Service Control Manager.
 *        Blocks until SCM dispatches ServiceMain and the agent exits.
 *        Returns 0 on clean stop, non-zero on dispatcher / init failure.
 */
int run_as_service(void);

/**
 * @brief Foreground / console mode — same loop without SCM.
 *        Ctrl+C handler triggers stop. Returns the agent exit code.
 */
int run_as_console(void);

/**
 * @brief Request the collection loop to exit.
 *        Safe to call from SCM control handler or console signal handler.
 */
void request_stop(void);

/**
 * @brief Returns non-zero when a stop has been requested.
 *        The loop polls this between iterations.
 */
int  stop_requested(void);

#endif
