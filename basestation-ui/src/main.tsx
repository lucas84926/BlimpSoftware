import { render } from "preact";
import "@fontsource-variable/archivo";
import "@fontsource-variable/jetbrains-mono";
import "./styles/theme.css";
import { App } from "./app.tsx";
import { connect } from "./net/ws.ts";

connect();
render(<App />, document.getElementById("app")!);
