
#include "Application.h"

#include <TGeoVolume.h>
#include <TH1D.h>
#include <TH2D.h>
#include <TRestGDMLParser.h>
#include <TRestGeant4Event.h>
#include <TRestGeant4Metadata.h>
#include <TRestGeant4PhysicsLists.h>
#include <TRestGeant4Track.h>
#include <TRestRun.h>

#include <G4RunManager.hh>
#include <G4UImanager.hh>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "ActionInitialization.h"
#include "CommandLineSetup.h"
#include "DetectorConstruction.h"
#include "EventAction.h"
#include "PhysicsList.h"
#include "PrimaryGeneratorAction.h"
#include "RunAction.h"
#include "SimulationManager.h"
#include "SteppingAction.h"
#include "TrackingAction.h"

#ifdef G4VIS_USE
#include "G4VisExecutive.hh"
#endif

#ifdef G4UI_USE
#include "G4UIExecutive.hh"
#endif

using namespace std;

void Application::Run(const CommandLineParameters& commandLineParameters) {
    auto simulationManager = new SimulationManager();

    Int_t& biasing = simulationManager->fBiasing;

    auto biasingSpectrum = simulationManager->biasingSpectrum;
    auto angularDistribution = simulationManager->biasingSpectrum;
    auto spatialDistribution = simulationManager->spatialDistribution;

    Bool_t saveAllEvents;

    Int_t nEvents;

    auto start_time = chrono::steady_clock::now();

    const auto originalDirectory = filesystem::current_path();

    cout << "Current working directory: " << originalDirectory << endl;

    CommandLineSetup::Print(commandLineParameters);

    /// Separating relative path and pure RML filename
    char* inputConfigFile = const_cast<char*>(commandLineParameters.rmlFile.Data());

    if (!TRestTools::CheckFileIsAccessible(inputConfigFile)) {
        cout << "Input rml file: " << inputConfigFile << " not found, please check file name" << endl;
        exit(1);
    }

    const auto [inputRmlPath, inputRmlClean] = TRestTools::SeparatePathAndName(inputConfigFile);

    if (!filesystem::path(inputRmlPath).empty()) {
        filesystem::current_path(inputRmlPath);
    }

    simulationManager->fRestGeant4Metadata = new TRestGeant4Metadata(inputRmlClean.c_str());
    simulationManager->fRestGeant4Metadata->SetGeant4Version(TRestTools::Execute("geant4-config --version"));

    if (!commandLineParameters.geometryFile.IsNull()) {
        simulationManager->fRestGeant4Metadata->SetGdmlFilename(commandLineParameters.geometryFile.Data());
    }

    // We need to process and generate a new GDML for several reasons.
    // 1. ROOT6 has problem loading math expressions in gdml file
    // 2. We allow file entities to be http remote files
    // 3. We retrieve the GDML and materials versions and associate to the
    // corresponding TRestGeant4Metadata members
    // 4. We support the use of system variables ${}
    auto gdml = new TRestGDMLParser();

    // This call will generate a new single file GDML output
    gdml->Load((string)simulationManager->fRestGeant4Metadata->GetGdmlFilename());

    // We redefine the value of the GDML file to be used in DetectorConstructor.
    simulationManager->fRestGeant4Metadata->SetGdmlFilename(gdml->GetOutputGDMLFile());
    simulationManager->fRestGeant4Metadata->SetGeometryPath("");

    simulationManager->fRestGeant4Metadata->SetGdmlReference(gdml->GetGDMLVersion());
    simulationManager->fRestGeant4Metadata->SetMaterialsReference(gdml->GetEntityVersion("materials"));

    simulationManager->fRestGeant4PhysicsLists = new TRestGeant4PhysicsLists(inputRmlClean.c_str());

    simulationManager->fRestRun = new TRestRun();
    simulationManager->fRestRun->LoadConfigFromFile(inputRmlClean);

    if (!commandLineParameters.outputFile.IsNull()) {
        simulationManager->fRestRun->SetOutputFileName(commandLineParameters.outputFile.Data());
    }

    filesystem::current_path(originalDirectory);

    TString runTag = simulationManager->fRestRun->GetRunTag();
    if (runTag == "Null" || runTag == "")
        simulationManager->fRestRun->SetRunTag(simulationManager->fRestGeant4Metadata->GetTitle());

    simulationManager->fRestRun->SetRunType("restG4");

    simulationManager->fRestRun->AddMetadata(simulationManager->fRestGeant4Metadata);
    simulationManager->fRestRun->AddMetadata(simulationManager->fRestGeant4PhysicsLists);
    simulationManager->fRestRun->PrintMetadata();

    simulationManager->fRestRun->FormOutputFile();

    simulationManager->fRestGeant4Event = new TRestGeant4Event();
    simulationManager->fRestGeant4SubEvent = new TRestGeant4Event();
    simulationManager->fRestRun->AddEventBranch(simulationManager->fRestGeant4SubEvent);

    simulationManager->fRestGeant4Track = new TRestGeant4Track();

    biasing = simulationManager->fRestGeant4Metadata->GetNumberOfBiasingVolumes();
    for (int i = 0; i < biasing; i++) {
        TString spectrumName = "Bias_Spectrum_" + TString(Form("%d", i));
        TString angDistName = "Bias_Angular_Distribution_" + TString(Form("%d", i));
        TString spatialDistName = "Bias_Spatial_Distribution_" + TString(Form("%d", i));

        Double_t maxEnergy = simulationManager->fRestGeant4Metadata->GetBiasingVolume(i).GetMaxEnergy();
        Double_t minEnergy = simulationManager->fRestGeant4Metadata->GetBiasingVolume(i).GetMinEnergy();
        auto nBins = (Int_t)(maxEnergy - minEnergy);

        Double_t biasSize =
            simulationManager->fRestGeant4Metadata->GetBiasingVolume(i).GetBiasingVolumeSize();
        TString biasType = simulationManager->fRestGeant4Metadata->GetBiasingVolume(i).GetBiasingVolumeType();

        cout << "Initializing biasing histogram : " << spectrumName << endl;
        biasingSpectrum[i] = new TH1D(spectrumName, "Biasing gamma spectrum", nBins, minEnergy, maxEnergy);
        angularDistribution[i] = new TH1D(angDistName, "Biasing angular distribution", 150, 0, M_PI / 2);

        if (biasType == "virtualSphere")
            spatialDistribution[i] =
                new TH2D(spatialDistName, "Biasing spatial (virtualSphere) distribution ", 100, -M_PI, M_PI,
                         100, 0, M_PI);
        else if (biasType == "virtualBox")
            spatialDistribution[i] =
                new TH2D(spatialDistName, "Biasing spatial (virtualBox) distribution", 100, -biasSize / 2.,
                         biasSize / 2., 100, -biasSize / 2., biasSize / 2.);
        else
            spatialDistribution[i] =
                new TH2D(spatialDistName, "Biasing spatial distribution", 100, -1, 1, 100, -1, 1);
    }

    // choose the Random engine
    CLHEP::HepRandom::setTheEngine(new CLHEP::RanecuEngine);
    long seed = simulationManager->fRestGeant4Metadata->GetSeed();
    CLHEP::HepRandom::setTheSeed(seed);

    auto runManager = new G4RunManager;

    auto detector = new DetectorConstruction(simulationManager);

    runManager->SetUserInitialization(detector);
    runManager->SetUserInitialization(new PhysicsList(simulationManager->fRestGeant4PhysicsLists));

    runManager->SetUserInitialization(new ActionInitialization(simulationManager));

    auto step = (SteppingAction*)G4RunManager::GetRunManager()->GetUserSteppingAction();
    auto primaryGenerator =
        (PrimaryGeneratorAction*)G4RunManager::GetRunManager()->GetUserPrimaryGeneratorAction();

    runManager->Initialize();

    G4UImanager* UI = G4UImanager::GetUIpointer();

#ifdef G4VIS_USE
    G4VisManager* visManager = new G4VisExecutive;
    visManager->Initialize();
#endif

    nEvents = simulationManager->fRestGeant4Metadata->GetNumberOfEvents();
    // We pass the volume definition to Stepping action so that it records gammas
    // entering in We pass also the biasing spectrum so that gammas energies
    // entering the volume are recorded
    if (biasing) {
        step->SetBiasingVolume(simulationManager->fRestGeant4Metadata->GetBiasingVolume(biasing - 1));
        step->SetBiasingSpectrum(biasingSpectrum[biasing - 1]);
        step->SetAngularDistribution(angularDistribution[biasing - 1]);
        step->SetSpatialDistribution(spatialDistribution[biasing - 1]);
    }

    time_t systime = time(nullptr);
    simulationManager->fRestRun->SetStartTimeStamp((Double_t)systime);

    cout << "Number of events : " << nEvents << endl;
    if (nEvents > 0)  // batch mode
    {
        G4String command = "/tracking/verbose 0";
        UI->ApplyCommand(command);
        command = "/run/initialize";
        UI->ApplyCommand(command);

        char tmp[256];
        sprintf(tmp, "/run/beamOn %d", nEvents);

        command = tmp;
        UI->ApplyCommand(command);

        simulationManager->fRestRun->GetOutputFile()->cd();

        if (biasing) {
            cout << "Biasing id: " << biasing - 1 << endl;
            step->GetBiasingVolume().PrintBiasingVolume();
            cout << "Number of events that reached the biasing volume : "
                 << (Int_t)(biasingSpectrum[biasing - 1]->Integral()) << endl;
            cout << endl;
            cout << endl;
            biasing--;
        }
        while (biasing) {
            simulationManager->fRestGeant4Metadata->RemoveParticleSources();

            auto src = new TRestGeant4ParticleSource();
            src->SetParticleName("gamma");
            src->SetEnergyDistType("TH1D");
            src->SetAngularDistType("TH1D");
            simulationManager->fRestGeant4Metadata->AddParticleSource(src);

            // We set the spectrum from previous biasing volume inside the primary generator
            primaryGenerator->SetSpectrum(biasingSpectrum[biasing]);
            // And we set the angular distribution
            primaryGenerator->SetAngularDistribution(angularDistribution[biasing]);

            // We re-define the generator inside restG4Metadata to be launched from the biasing volume
            simulationManager->fRestGeant4Metadata->SetGeneratorType(
                simulationManager->fRestGeant4Metadata->GetBiasingVolume(biasing).GetBiasingVolumeType());
            double size =
                simulationManager->fRestGeant4Metadata->GetBiasingVolume(biasing).GetBiasingVolumeSize();
            simulationManager->fRestGeant4Metadata->SetGeneratorSize(TVector3(size, size, size));
            // simulationManager->fRestGeant4Metadata->GetBiasingVolume( biasing-1 ).PrintBiasingVolume();

            // Defining biasing the number of event to be re-launched
            Double_t biasingFactor =
                simulationManager->fRestGeant4Metadata->GetBiasingVolume(biasing - 1).GetBiasingFactor();
            cout << "Biasing id: " << biasing - 1 << ", Events to be launched : "
                 << (Int_t)(biasingSpectrum[biasing]->Integral() * biasingFactor) << endl;

            sprintf(tmp, "/run/beamOn %d", (Int_t)(biasingSpectrum[biasing]->Integral() * biasingFactor));
            command = tmp;

            simulationManager->fRestRun->GetOutputFile()->cd();

            biasingSpectrum[biasing]->Write();
            angularDistribution[biasing]->Write();
            spatialDistribution[biasing]->Write();

            // We pass the volume definition to Stepping action so that it records
            // gammas entering in We pass also the biasing spectrum so that gammas
            // energies entering the volume are recorded
            if (biasing) {
                step->SetBiasingVolume(simulationManager->fRestGeant4Metadata->GetBiasingVolume(biasing - 1));
                step->SetBiasingSpectrum(biasingSpectrum[biasing - 1]);
                step->SetAngularDistribution(angularDistribution[biasing - 1]);
                step->SetSpatialDistribution(spatialDistribution[biasing - 1]);
            }
            UI->ApplyCommand(command);
            step->GetBiasingVolume().PrintBiasingVolume();
            cout << "Number of events that reached the biasing volume : "
                 << (Int_t)(biasingSpectrum[biasing - 1]->Integral()) << endl;
            cout << endl;
            cout << endl;
            biasing--;
        }
    }

    else if (nEvents == 0)  // define visualization and UI terminal for interactive mode
    {
        cout << "Entering vis mode.." << endl;
#ifdef G4UI_USE
        auto ui = new G4UIExecutive(commandLineParameters.argc, commandLineParameters.argv);
#ifdef G4VIS_USE
        cout << "Executing G4 macro : /control/execute macros/vis.mac" << endl;
        UI->ApplyCommand("/control/execute macros/vis.mac");
#endif
        ui->SessionStart();
        delete ui;
#endif
    }

    else  // nEvents == -1
    {
        cout << "++++++++++ ERROR +++++++++" << endl;
        cout << "++++++++++ ERROR +++++++++" << endl;
        cout << "++++++++++ ERROR +++++++++" << endl;
        cout << "The number of events to be simulated was not recognized properly!" << endl;
        cout << "Make sure you did not forget the number of events entry in TRestGeant4Metadata." << endl;
        cout << endl;
        cout << " ... or the parameter is properly constructed/interpreted." << endl;
        cout << endl;
        cout << "It should be something like : " << endl;
        cout << endl;
        cout << R"( <parameter name ="nEvents" value="100"/>)" << endl;
        cout << "++++++++++ ERROR +++++++++" << endl;
        cout << "++++++++++ ERROR +++++++++" << endl;
        cout << "++++++++++ ERROR +++++++++" << endl;
        cout << endl;
    }
    simulationManager->fRestRun->GetOutputFile()->cd();

#ifdef G4VIS_USE
    delete visManager;
#endif

    // job termination
    delete runManager;

    systime = time(nullptr);
    simulationManager->fRestRun->SetEndTimeStamp((Double_t)systime);
    TString filename = TRestTools::ToAbsoluteName(simulationManager->fRestRun->GetOutputFileName().Data());

    simulationManager->fRestRun->UpdateOutputFile();
    simulationManager->fRestRun->CloseFile();
    simulationManager->fRestRun->PrintMetadata();
    delete simulationManager->fRestRun;

    delete simulationManager->fRestGeant4Event;
    delete simulationManager->fRestGeant4SubEvent;
    delete simulationManager->fRestGeant4Track;
    ////////// Writing the geometry in TGeoManager format to the ROOT file
    ////////// Need to fork and do it in child process, to prevent occasional seg.fault
    pid_t pid;
    pid = fork();
    if (pid < 0) {
        perror("fork error:");
        exit(1);
    }
    // child process
    if (pid == 0) {
        // writing the geometry object
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        REST_Display_CompatibilityMode = true;

        // We wait the father process ends properly
        sleep(5);

        // Then we just add the geometry
        auto file = new TFile(filename, "update");
        TGeoManager* geoManager = gdml->CreateGeoManager();

        file->cd();
        geoManager->SetName("Geometry");
        geoManager->Write();
        file->Close();
        exit(0);
    }
    // father process
    else {
        int stat_val = 0;
        pid_t child_pid;

        printf("Writing geometry ... \n");
    }

    cout << "============== Generated file: " << filename << " ==============" << endl;
    auto end_time = chrono::steady_clock::now();
    cout << "Elapsed time: " << chrono::duration_cast<chrono::seconds>(end_time - start_time).count()
         << " seconds" << endl;
}
