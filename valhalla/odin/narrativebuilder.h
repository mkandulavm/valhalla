#ifndef VALHALLA_ODIN_NARRATIVEBUILDER_H_
#define VALHALLA_ODIN_NARRATIVEBUILDER_H_

#include <vector>

#include <valhalla/proto/trippath.pb.h>

#include <valhalla/odin/maneuver.h>

namespace valhalla {
namespace odin {

class NarrativeBuilder {
 public:

  static void Build(std::list<Maneuver>& maneuvers);

 protected:
  NarrativeBuilder();

  static void FormStartInstruction(Maneuver& maneuver);

  static void FormDestinationInstruction(Maneuver& maneuver);

  static void FormContinueInstruction(Maneuver& maneuver);

  static void FormTurnInstruction(Maneuver& maneuver);

  static void FormUturnInstruction(Maneuver& maneuver);

  static void FormRampInstruction(Maneuver& maneuver);

  static void FormExitInstruction(Maneuver& maneuver);

  static void FormStayInstruction(Maneuver& maneuver);

  static void FormMergeInstruction(Maneuver& maneuver);

  static void FormEnterRoundaboutInstruction(Maneuver& maneuver);

  static void FormExitRoundaboutInstruction(Maneuver& maneuver);

  static void FormEnterFerryInstruction(Maneuver& maneuver);

  static void FormExitFerryInstruction(Maneuver& maneuver);

  static std::string FormCardinalDirection(
      TripDirections_Maneuver_CardinalDirection cardinal_direction);

  static std::string FormTurnTypeInstruction(TripDirections_Maneuver_Type type);

};

}
}

#endif  // VALHALLA_ODIN_NARRATIVEBUILDER_H_
