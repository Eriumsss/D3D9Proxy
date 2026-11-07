RETAIL FUNCTION TRACER -- PRESETS

Copy one over retail_trace.ini (next to ConquestTrace.dll), run the game, then
read retail_story.txt.

  everything.ini    EVERY address we have decoded -- all 1,680 ported functions,
                    every call counted, call tree on. THE DEFAULT ANSWER to
                    "how does the game actually run". Start here.

  everything_all.ini  All 30,584 catalogued entry points, coverage mode. The
                    widest net there is: one trap per function, disarmed after,
                    so it costs almost nothing and tells you which of the whole
                    corpus this session touched. Use to find code we have NOT
                    decoded yet that runs during something you care about.

  render.ini        Everything ported, but with the render phases anchored so
                    frame boundaries are findable by eye. Same capture as
                    everything.ini -- anchors only affect the reading.

  shaders.ini       Narrow: how the shader bin is read and shaders get bound.
                    Only when the full trace is too much to hold at once.

  teardown.ini      Narrow: every shutdown / destroy / free / release path.

WHY THE DEFAULT IS "EVERYTHING"
Topics and Lanes narrow what gets ARMED, and 1,317 of the 1,680 functions we
have decoded carry no topic at all -- so a topic filter hides 78% of what we
know. Capture everything; use retail_story.txt's BY LANE and BY TOPIC sections
to read it. The narrow presets exist for when you already know the question.

WHAT COMES OUT
  retail_calls.log        every call, in time order, indented into a call tree
  retail_story.txt        the same data as a readable sequence:
                            1. EXECUTION ORDER  - first appearance, in order
                            2. BY LANE          - grouped by the port file
                            3. BY TOPIC         - grouped by story
                          each group also lists what NEVER RAN, which is half
                          the result
  retail_coverage.csv     per function: hits, first frame, first ms, our port site
  retail_trace_status.txt live counters

NOTHING IS EVER LOST SILENTLY
  - ring overflow prints a !!!!!! banner naming how many records were lost
  - a function retired by the hot governor prints !! HOT with its name and count
  - functions the interlock refused are named at arm time
  - functions that never ran are listed explicitly per group
