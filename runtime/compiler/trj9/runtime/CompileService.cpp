#include "runtime/CompileService.hpp"
#include "j9.h"
#include "control/CompilationRuntime.hpp"
#include "control/JaasCompilationThread.hpp"
#include "env/j9methodServer.hpp"

static J9Method *ramMethodFromRomMethod(J9JITConfig *jitConfig, J9VMThread *vmThread,
                                       const J9ROMClass* romClass, const J9ROMMethod* romMethod,
                                       void* classChainC, void* classChainCL)
   {
   // Acquire vm access within this scope, variable is intentionally unused
   VMAccessHolder access(vmThread);

   TR_J9VMBase *fej9 = TR_J9VMBase::get(jitConfig, vmThread);
   TR_J9SharedCache *cache = fej9->sharedCache();
   J9ClassLoader *CL = (J9ClassLoader*)cache->persistentClassLoaderTable()->lookupClassLoaderAssociatedWithClassChain(classChainCL);
   if (CL)
      {
      J9Class *ramClass = (J9Class*)cache->lookupClassFromChainAndLoader((uintptrj_t *)classChainC, CL);
      if (ramClass)
         {
         J9Method *ramMethods = ramClass->ramMethods;
         for (int32_t i = 0; i < romClass->romMethodCount; i++)
            {
            J9Method *curMethod = ramMethods + i;
            J9ROMMethod *curROMMethod = J9_ROM_METHOD_FROM_RAM_METHOD(curMethod);
            if (curROMMethod == romMethod)
               return curMethod;
            }
         }
      }
   return NULL;
   }

static void doRemoteCompile(J9JITConfig* jitConfig, J9VMThread* vmThread,
   J9ROMClass* romClass, const J9ROMMethod* romMethod,
   J9Method* ramMethod, J9Class *clazz, JAAS::J9ServerStream *rpc, TR_Hotness optLevel,
   TR::IlGeneratorMethodDetails *clientDetails,
   J9::IlGeneratorMethodDetailsType methodDetailsType,
   J9Method *methodsOfClass)
   {
   J9UTF8 *methodNameUTF = J9ROMNAMEANDSIGNATURE_NAME(&romMethod->nameAndSignature);
   std::string methodNameStr((const char*)methodNameUTF->data, (size_t)methodNameUTF->length);
   const char *methodName = methodNameStr.c_str();
   J9UTF8 *classNameUTF = J9ROMCLASS_CLASSNAME(romClass);
   std::string classNameStr((const char*)classNameUTF->data, (size_t)classNameUTF->length);
   const char *className = classNameStr.c_str();

   // Acquire vm access within this scope, variable is intentionally unused
   VMAccessHolder access(vmThread);

   PORT_ACCESS_FROM_JITCONFIG(jitConfig);

   if (TR::Options::getVerboseOption(TR_VerboseJaas))
      TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
         "Server received request to compile %s.%s @ %s", className, methodName, TR::Compilation::getHotnessName(optLevel));

   TR::CompilationInfo * compInfo = getCompilationInfo(jitConfig);
   TR_J9VMBase *fe = TR_J9VMBase::get(jitConfig, vmThread);
   if (ramMethod)
      {
      bool queued = false;
      TR_CompilationErrorCode compErrCode = compilationFailure;
      TR_MethodEvent event;
      event._eventType = TR_MethodEvent::RemoteCompilationRequest;
      event._j9method = ramMethod;
      event._oldStartPC = 0;
      event._vmThread = vmThread;
      event._classNeedingThunk = 0;
      event._jaasClientOptLevel = optLevel;
      bool newPlanCreated;
      IDATA result = 0;
      TR_OptimizationPlan *plan = TR::CompilationController::getCompilationStrategy()->processEvent(&event, &newPlanCreated);

      // if the controller decides to compile this method, trigger the compilation
      if (plan)
         {
         TR::IlGeneratorMethodDetails details;
         *(uintptr_t*)clientDetails = 0; // smash remote vtable pointer to catch bugs early
         TR::IlGeneratorMethodDetails &remoteDetails = details.createRemoteMethodDetails(*clientDetails, methodDetailsType, ramMethod, romClass, romMethod, clazz, methodsOfClass);
         result = (IDATA)compInfo->compileRemoteMethod(vmThread, remoteDetails, romMethod, romClass, 0, &compErrCode, &queued, plan, rpc);

         if (newPlanCreated)
            {
            if (!queued)
               TR_OptimizationPlan::freeOptimizationPlan(plan);

            // If the responder has been handed over to the compilation thread, the compErrCode should be compilationInProgress
            if (compErrCode == compilationInProgress)
               {
               // This should be the only path in which we do not call finish (the compilation thread will do that instead)
               if (TR::Options::getVerboseOption(TR_VerboseJaas))
                  TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                     "Server queued compilation for %s.%s", className, methodName);
               }
            else
               {
               if (TR::Options::getVerboseOption(TR_VerboseJaas))
                  TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                     "Server failed to queue compilation for %s.%s", className, methodName);
               fe->jitPersistentFree(romClass);
               rpc->finishCompilation(compErrCode);
               }
            }
         else
            {
            if (TR::Options::getVerboseOption(TR_VerboseJaas))
               TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                  "Server failed to compile %s.%s because a new plan could not be created.", className, methodName);
            fe->jitPersistentFree(romClass);
            rpc->finishCompilation(compilationFailure);
            }
         }
      else
         {
         if (TR::Options::getVerboseOption(TR_VerboseJaas))
            TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
               "Server failed to compile %s.%s because no memory was available to create an optimization plan.", className, methodName);
         fe->jitPersistentFree(romClass);
         rpc->finishCompilation(compilationFailure);
         }
      }
   else // !method
      {
      if (TR::Options::getVerboseOption(TR_VerboseJaas))
         TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
            "Server couldn't find ramMethod for romMethod %s.%s .", className, methodName);
      fe->jitPersistentFree(romClass);
      rpc->finishCompilation(compilationFailure);
      }
   }

void J9CompileDispatcher::compile(JAAS::J9ServerStream *stream)
   {
   try
      {
      auto req = stream->read<uint64_t, std::string, uint32_t, J9Method *, J9Class*, TR_Hotness, std::string, 
                              J9::IlGeneratorMethodDetailsType, std::vector<TR_OpaqueClassBlock*>, J9Method*,
                              TR_OpaqueClassBlock *, int32_t>();

      PORT_ACCESS_FROM_JITCONFIG(_jitConfig);
      TR_J9VMBase *fej9 = TR_J9VMBase::get(_jitConfig, _vmThread);
      TR_J9SharedCache *cache = fej9->sharedCache();
      uint64_t clientId = std::get<0>(req);
      stream->setClientId(clientId);
      std::string romClassStr = std::get<1>(req);
      J9ROMClass *romClass = TR_ResolvedJ9JAASServerMethod::romClassFromString(romClassStr, fej9->_compInfo->persistentMemory());
      uint32_t romMethodOffset = std::get<2>(req);
      J9ROMMethod *romMethod = (J9ROMMethod*)((uint8_t*) romClass + romMethodOffset);
      J9Method *ramMethod = std::get<3>(req);
      J9Class *clazz = std::get<4>(req);
      TR_Hotness opt = std::get<5>(req);
      std::string detailsStr = std::get<6>(req);
      TR::IlGeneratorMethodDetails *details = (TR::IlGeneratorMethodDetails*) &detailsStr[0];
      auto detailsType = std::get<7>(req);
      TR::CompilationInfo * compInfo = getCompilationInfo(jitConfig);
      auto &unloadedClasses = std::get<8>(req);
      J9Method *methodsOfClass = std::get<9>(req);
      TR_OpaqueClassBlock *baseComponentClass = std::get<10>(req);
      int32_t numDims = std::get<11>(req);
      // If new classes have been unloaded at the client, 
      // delete relevant entries from the persistent caches we hold per client
      //if (unloadedClasses.size() != 0)
         {
         OMR::CriticalSection compilationMonitorLock(compInfo->getCompilationMonitor());
         auto clientSession = compInfo->getClientSessionHT()->findClientSession(clientId); // cannot throw
         if (clientSession)
            {
            // Cache the ROMClass for the method to be compiled if not already cached
            if (!JaasHelpers::getRemoteROMClassIfCached(clientSession, clazz))
               JaasHelpers::cacheRemoteROMClass(clientSession, clazz, romClass, methodsOfClass, baseComponentClass, numDims);
            // This could be an expensive operation and we are holding the compilation monitor
            // Maybe we should create another monitor
            if (unloadedClasses.size() != 0)
               clientSession->processUnloadedClasses(unloadedClasses);
            clientSession->decInUse();
            }
         }

      doRemoteCompile(_jitConfig, _vmThread, romClass, romMethod, ramMethod, clazz, stream, opt, details, detailsType, methodsOfClass);
      }
   catch (const JAAS::StreamFailure &e)
      {
      if (TR::Options::getVerboseOption(TR_VerboseJaas))
         TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS, "Stream failed in server compilation dispatcher thread: %s", e.what());
      stream->cancel();
      }
   }
