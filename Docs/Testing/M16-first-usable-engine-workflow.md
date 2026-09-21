# M16 First Usable Engine Workflow Manual Test

## Purpose

新規WorkspaceからBlank 3D Projectを作成し、Default Scene編集、Project Files操作、Play／Stop、
Game Build、Standalone Package公開、Standalone起動までを一つの制作Loopとして確認する。

一般Asset Import／Cook、3D Viewport、Game Rendering、Sound、Effect、Physics、ECS改良は対象外とする。

## Preconditions

1. Windows x64、Visual Studio 2026、CMake 4.2.0以上、PowerShell 7を使用する。
2. Repository Rootで`README.md`のDependency Restore手順に従い、`ToolRoot`、`InstallRoot`、
   `GitExecutable`を明示して`Tools/Dependencies/RestoreVcpkg.ps1`を実行済みにする。
3. `cmake --preset windows-vs2026`を実行する。
4. `cmake --build --preset windows-vs2026-debug`を実行する。
5. `cmake --build --preset windows-vs2026-development`を実行する。
6. `cmake --build --preset windows-vs2026-release`を実行する。
7. Test専用の空DirectoryをProject作成先として用意する。

## Create and Open a Blank Game

1. `out/build/windows-vs2026/bin/Debug/CueProjectHubTool.exe`を起動する。
2. `新しいProject (Ctrl+N)`を押す。
3. `作成先Folder`、単一Folder名の`Project名`、任意の`表示名`を入力する。
4. `Template`で`Blank 3D`を選択し、`作成`を押す。
5. 一覧にProjectが`利用可能`として表示されることを確認する。
6. Projectを選択して`Editorで開く`を押す。
7. `Assets/Source/Scenes/Default.cuescene`が自動的に開くことを確認する。

Blank 3Dは、Version付き`CueProject.json`、Default Scene、3構成のCMake Preset、最小Game Moduleを生成する。
外部AssetまたはProject固有の第三者Codeは生成しない。

## Edit and Save the Default Scene

1. `Hierarchy`の`Objectを追加`を押し、最初のObjectを作成する。
2. `Ctrl`を押しながら最初のObjectを再度選択し、Selectionを解除してScene Rootへ戻す。
3. `Objectを追加`をもう一度押し、最初のObjectとSiblingになる二個目のObjectを作成する。
4. 二個目のObjectを編集対象にし、`Inspector`で名前、Translation／Rotation／Scaleを編集し、Parentを最初のObjectへ変更する。
5. `Transformを適用`を押す。
6. Editor終了を要求し、未保存Sceneに対する`保存`／`破棄`／`キャンセル`確認が表示されることを確認して`キャンセル`を選ぶ。
7. `編集`Menuまたは`Ctrl+Z`／`Ctrl+Y`でUndo／Redoし、Hierarchy、Inspector、Selectionが対応する状態へ戻ることを確認する。
8. `ファイル`Menuの`保存`または`Ctrl+S`でDefault Sceneを保存する。
9. Editorを閉じてProject Hubから同じProjectを再Openし、Object Identityと編集内容が維持されることを確認する。

## Operate Project Files

1. `Files` Windowで`更新`を押す。
2. `新規フォルダー`と`新規ファイル`を作成する。
3. 作成Entryを選択してRename、Copy、Moveを実行する。
4. `削除`からPreviewを確認し、`Trashへ移動`を選ぶ。
5. Trash一覧から対象を選び`復元`する。
6. Active SceneまたはProject Root外を対象にした操作が拒否され、Editorと既存Fileが維持されることを確認する。

より詳細なFiles操作とNative Dialogの確認は、
`Docs/Testing/M13-files-workflow-manual-test.md`と`Docs/Testing/M13-native-file-dialog-manual-test.md`を使用する。

## Play and Stop

1. `Runtime` Windowの`Play (F5)`を押す。
2. 状態が実行中になり、Runtime Consoleへ開始Logが表示されることを確認する。
3. `Stop (Shift+F5)`を押し、停止後もSelection、Dirty State、Authoring Scene内容が変わらないことを確認する。
4. 再びPlay／Stopできることを確認する。

現在のPlayはEditor内のHeadless Runtime Worldを動かす。3D Game ViewまたはGame Renderingは表示しない。

## Build, Package, and Run

1. `Build Package Run` Windowで`Configuration`を`Debug`にする。
2. `Build & Package`を押す。
3. 状態がBuild、Package、`Package Ready`の順に進むことを確認する。
4. `Published Package`にProject、Configuration、Files、Destinationが表示されることを確認する。
5. `Run`を押し、Standalone Runtime Windowが固定色を表示して起動することを確認する。
6. Runtime Windowを閉じるか`Stop`を押し、Editorに子Processが残らないことを確認する。
7. DevelopmentとReleaseも同じ手順でBuild／Packageできることを確認する。

Packageは`Generated/Packages/<Configuration>/<operation-id>`へ新規公開される。既存Packageを上書きせず、
`CuePackage.json`、`CueRuntimeHost.exe`、Game Module、`Game/CueGameModule.metadata.json`、Runtime Project Data、
Runtime Scene Dataだけで起動する。

## Failure and Recovery

1. `Source/Game/GameModule.cpp`へ一時的なC++構文Errorを追加する。
2. `Build & Package`を実行し、Build StageでFailedになり、新しいPackageが公開されないことを確認する。
3. 直前の成功Package表示とDirectoryが維持されることを確認する。
4. Sourceを元に戻して`Retry`を押し、新しいPackageが`Package Ready`になることを確認する。
5. Scene内Objectの名前またはTransformを変更して適用し、SceneをDirtyにする。
6. Editor終了を要求し、`キャンセル`を選んでEditorが継続することを確認する。
7. 再度終了を要求して`保存`を選び、保存完了後にEditorが終了することを確認する。Project Hubから同じProjectを再Openする。
8. Sceneを再びDirtyにして終了を要求し、`破棄`を選んでEditorが終了することを確認する。Project Hubから同じProjectを再Openする。
9. Build中にEditor終了を要求し、`Editorへ戻る`で処理とEditorが継続することを確認する。再度終了を要求し、`停止して終了`で子Processと作業が終了することを確認する。
10. Project Hubから同じProjectを再Openし、Package中について手順9を繰り返す。
11. Project Hubから同じProjectを再Openし、同じSessionで`Build & Package`を成功させてから`Run`を開始し、
    Run中について手順9を繰り返す。各Stageの開始前にEditorを起動し直し、前の終了判断に依存しないSessionで確認する。

Build Errorの詳細は`Build Package Run` Windowの`Build Diagnostics`と`Build Output`で確認する。Package失敗時は不完全な
最終Destinationを成功扱いせず、Rollbackの即時再試行にも失敗したStagingがある場合だけ同Windowの`Recovery Staging`へ
Project Root相対Locatorを表示して保持する。

## Acceptance Checklist

- [ ] Blank 3D ProjectとDefault Sceneを新規作成できる
- [ ] Hierarchy／Inspector編集、Undo／Redo、Save、再Openが成功する
- [ ] Project内File操作とTrash復元が成功し、Root外操作を拒否する
- [ ] Play／Stop後もAuthoring状態を維持する
- [ ] Debug／Development／ReleaseのGame BuildとPackageが成功する
- [ ] Standalone Runtimeを起動して停止できる
- [ ] Build失敗でPackageを公開せず、Source復旧後にRetryできる
- [ ] Editor終了後にBuildまたはRuntime子Processが残らない

## Known Limitations

- Windows x64、Visual Studio 2026、DirectX 12だけを検証対象とする。
- Runtimeの可視出力は固定色Clearで、Game Rendering、3D Viewport、Gizmoを含まない。
- Runtime Data変換はDefault／Startup Sceneだけを扱い、一般Asset Import／Cookを含まない。
- Blank Game ModuleはABI接続用の最小実装で、利用者向けScripting、Hot Reload、Gameplay Frameworkを含まない。
- Sound、Effect、Physics、Prefab、Runtime Packaging Installer、Code Signingを含まない。
