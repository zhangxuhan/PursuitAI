// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PursuitAI : ModuleRules
{
	public PursuitAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// The playable level's chasers are AI-possessed characters, so their
			// CharacterMovementComponent actually moves. Only the play map uses it; the RL
			// environment has no controller and no physics.
			"AIModule",

			// Schola runtime modules. Schola is the core (spaces/points + the IAgent
			// inference interface), ScholaTraining owns the training-side environment
			// interfaces + gym connector manager, ScholaProtobuf provides the gRPC
			// connector used to talk to the Python trainer.
			"Schola",
			"ScholaTraining",
			"ScholaProtobuf",

			// Reusable sensors/actuators (MovementInputActuator, RayCastSensor) - the v2
			// character agent attaches the movement actuator and implements the sensor
			// interface against this module's interfaces.
			"ScholaInteractors",

			// Inference side, so a trained ONNX model can drive the same actor with no
			// Python attached. ScholaNNE is the NNE policy, ScholaInferenceUtils is the
			// Observe -> Think -> Act stepper, NNE is the engine runtime that loads
			// UNNEModelData.
			"ScholaNNE",
			"ScholaInferenceUtils",
			"NNE",

			// Used by the diagnostic bridge to read the plugin descriptor.
			"Projects"
		});
	}
}
