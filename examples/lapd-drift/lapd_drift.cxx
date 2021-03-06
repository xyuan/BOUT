/*******************************************************************************
 * 2-fluid equations
 * Same as Maxim's version of BOUT - simplified 2-fluid for benchmarking
 *******************************************************************************/

#include <bout.hxx>
#include <boutmain.hxx>

#include <initialprofiles.hxx>
#include <derivs.hxx>
#include <interpolation.hxx>
#include <invert_laplace.hxx>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// 2D initial profiles
Field2D Ni0, Ti0, Te0, Vi0, phi0, Ve0, rho0, Ajpar0;
Vector2D b0xcv; // for curvature terms

// 3D evolving fields
Field3D rho, Ni, Ajpar;

// Derived 3D variables
Field3D phi, Apar, Ve, jpar;

// Non-linear coefficients
Field3D nu, mu_i, kapa_Te, kapa_Ti;

// 3D total values
Field3D Nit, Tit, Tet, Vit;

// pressures
Field3D pei, pe;
Field2D pei0, pe0;

// Metric coefficients
Field2D Rxy, Bpxy, Btxy, hthe;

// parameters
BoutReal Te_x, Ti_x, Ni_x, Vi_x, bmag, rho_s, fmei, AA, ZZ;
BoutReal lambda_ei, lambda_ii;
BoutReal nu_hat, mui_hat, wci, nueix, nuiix;
BoutReal beta_p;
BoutReal nuIonNeutral; // Ion-neutral collision rate (normalised by wci)

// settings
bool estatic, ZeroElMass; // Switch for electrostatic operation (true = no Apar)

bool arakawa;   // Use Arakawa scheme for ExB advection
bool bout_exb;  // Use BOUT-06 expression for ExB velocity

BoutReal zeff, nu_perp;
bool evolve_rho,evolve_ni, evolve_ajpar;
BoutReal ShearFactor;

bool nonlinear;    // Include nonlinear terms

bool log_density;  // Evolve logarithm of the density

bool filter_z;     // Remove all but a single azimuthal mode-number
int filter_z_mode; // The azimuthal mode-number to keep

int phi_flags, apar_flags; // Inversion flags

bool niprofile;

bool evolve_source; // If true, evolve a source/sink profile
BoutReal source_response;  // Initial source response (inverse timescale) 
BoutReal source_converge;  // Timescale for convergence
Field2D Sn; // Density source (inverse timescale)
bool input_source; // Read Sn from the input file

// Communication object
FieldGroup comms;

int physics_init(bool restarting)
{
  Field2D I; // Shear factor 
  
  output.write("Solving 6-variable 2-fluid equations\n");

  /************* LOAD DATA FROM GRID FILE ****************/

  // Load 2D profiles (set to zero if not found)
  mesh->get(Ni0,    "Ni0");
  mesh->get(Ti0,    "Ti0");
  mesh->get(Te0,    "Te0");
  mesh->get(Vi0,    "Vi0");
  mesh->get(Ve0,    "Ve0");
  mesh->get(phi0,   "phi0");
  mesh->get(rho0,   "rho0");
  mesh->get(Ajpar0, "Ajpar0");

  // Load magnetic curvature term
  b0xcv.covariant = false; // Read contravariant components
  mesh->get(b0xcv, "bxcv"); // b0xkappa terms

  // Load metrics
  mesh->get(Rxy,  "Rxy");
  mesh->get(Bpxy, "Bpxy");
  mesh->get(Btxy, "Btxy");
  mesh->get(hthe, "hthe");
  mesh->get(mesh->dx,   "dpsi");
  mesh->get(I,    "sinty");
  mesh->get(mesh->zShift, "qinty");

  // Load normalisation values
  mesh->get(Te_x, "Te_x");
  mesh->get(Ti_x, "Ti_x");
  mesh->get(Ni_x, "Ni_x");
  mesh->get(bmag, "bmag");

  Ni_x *= 1.0e14;
  bmag *= 1.0e4;

  /*************** READ OPTIONS *************************/

  // Read some parameters
  Options *globalOptions = Options::getRoot();
  Options *options = globalOptions->getSection("2fluid");
  OPTION(options, AA, 2.0); // <=> options.get("AA", AA, 1.0);
  OPTION(options, ZZ, 1.0);

  OPTION(options, estatic,     false);
  OPTION(options, ZeroElMass,  false);
  OPTION(options, zeff,        1.0);
  OPTION(options, nu_perp,     0.0); 
  OPTION(options, ShearFactor, 1.0); 
  OPTION(options, nuIonNeutral, -1.);
  OPTION(options, arakawa,     false);
  OPTION(options, bout_exb,    false);
  
  OPTION(options, niprofile, false);
  OPTION(options, evolve_source, false);
  OPTION(options, source_response, 1.0);
  OPTION(options, source_converge, 100.);
  
  OPTION(options, input_source, false);

  OPTION(options, phi_flags,   0);
  OPTION(options, apar_flags,  0);

  OPTION(options, nonlinear, true);

  OPTION(options, log_density, false);
  if(log_density) {
    if(!nonlinear)
      output << "WARNING: logarithmic density => Nonlinear terms enabled\n";
    nonlinear = true; // Need to include the nonlinear terms
  }

  // Toroidal filtering
  OPTION(options, filter_z,          false);  // Filter a single n
  OPTION(options, filter_z_mode,     1);

  // Check for "evolve" inside variable sections
  (globalOptions->getSection("rho"))->get("evolve", evolve_rho,   true);
  (globalOptions->getSection("Ni"))->get("evolve", evolve_ni,    true);
  (globalOptions->getSection("Ajpar"))->get("evolve", evolve_ajpar, true);

  if(ZeroElMass)
    evolve_ajpar = false; // Don't need ajpar - calculated from ohm's law

  /************* SHIFTED RADIAL COORDINATES ************/

  if(mesh->ShiftXderivs) {
    ShearFactor = 0.0;  // I disappears from metric
    b0xcv.z += I*b0xcv.x;
  }

  /************** CALCULATE PARAMETERS *****************/

  rho_s = 1.02*sqrt(AA*Te_x)/ZZ/bmag;
  fmei  = 1./1836.2/AA;

  lambda_ei = 24.-log(sqrt(Ni_x)/Te_x);
  lambda_ii = 23.-log(ZZ*ZZ*ZZ*sqrt(2.*Ni_x)/pow(Ti_x, 1.5));
  wci       = 9.58e3*ZZ*bmag/AA;
  nueix     = 2.91e-6*Ni_x*lambda_ei/pow(Te_x, 1.5);
  nuiix     = 4.78e-8*pow(ZZ,4.)*Ni_x*lambda_ii/pow(Ti_x, 1.5)/sqrt(AA);
  nu_hat    = zeff*nueix/wci;

  if(nu_perp < 1.e-10) {
    mui_hat      = (3./10.)*nuiix/wci;
  } else
    mui_hat      = nu_perp;

  if(estatic) {
    beta_p    = 1.e-29;
  }else
    beta_p    = 4.03e-11*Ni_x*Te_x/bmag/bmag;

  Vi_x = wci * rho_s;

  output.write("Collisions: nueix = %e, nu_hat = %e\n", nueix, nu_hat);

  /************** PRINT Z INFORMATION ******************/
  
  BoutReal hthe0;
  if(mesh->get(hthe0, "hthe0") == 0) {
    output.write("    ****NOTE: input from BOUT, Z length needs to be divided by %e\n", hthe0/rho_s);
  }

  /************** SHIFTED GRIDS LOCATION ***************/

  // Velocities defined on cell boundaries
  Ajpar.setLocation(CELL_YLOW);

  // Apar and jpar too
  Apar.setLocation(CELL_YLOW); 
  jpar.setLocation(CELL_YLOW);

  /************** NORMALISE QUANTITIES *****************/

  output.write("\tNormalising to rho_s = %e\n", rho_s);

  // Normalise profiles
  Ni0  /= Ni_x/1.0e14;
  Ti0  /= Te_x;
  Te0  /= Te_x;
  phi0 /= Te_x;
  Vi0  /= Vi_x;

  // Normalise curvature term
  b0xcv.x /= (bmag/1e4);
  b0xcv.y *= rho_s*rho_s;
  b0xcv.z *= rho_s*rho_s;
  
  // Normalise geometry 
  Rxy /= rho_s;
  hthe /= rho_s;
  I *= rho_s*rho_s*(bmag/1e4)*ShearFactor;
  mesh->dx /= rho_s*rho_s*(bmag/1e4);

  // Normalise magnetic field
  Bpxy /= (bmag/1.e4);
  Btxy /= (bmag/1.e4);
  mesh->Bxy  /= (bmag/1.e4);

  // calculate pressures
  pei0 = (Ti0 + Te0)*Ni0;
  pe0 = Te0*Ni0;

  /**************** CALCULATE METRICS ******************/

  mesh->g11 = (Rxy*Bpxy)^2;
  mesh->g22 = 1.0 / (hthe^2);
  mesh->g33 = (I^2)*mesh->g11 + (mesh->Bxy^2)/mesh->g11;
  mesh->g12 = 0.0;
  mesh->g13 = -I*mesh->g11;
  mesh->g23 = -Btxy/(hthe*Bpxy*Rxy);
  
  mesh->J = hthe / Bpxy;
  
  mesh->g_11 = 1.0/mesh->g11 + ((I*Rxy)^2);
  mesh->g_22 = (mesh->Bxy*hthe/Bpxy)^2;
  mesh->g_33 = Rxy*Rxy;
  mesh->g_12 = Btxy*hthe*I*Rxy/Bpxy;
  mesh->g_13 = I*Rxy*Rxy;
  mesh->g_23 = Btxy*hthe*Rxy/Bpxy;

  /**************** SET EVOLVING VARIABLES *************/

  // Tell BOUT++ which variables to evolve
  // add evolving variables to the communication object
  if(evolve_rho) {
    bout_solve(rho, "rho");
    comms.add(rho);
  }else
    initial_profile("rho", rho);

  if(evolve_ni) {
    bout_solve(Ni, "Ni");
    comms.add(Ni);
  }else
    initial_profile("Ni", Ni);

  if(evolve_ajpar) {
    bout_solve(Ajpar, "Ajpar");
    comms.add(Ajpar);
  }else {
    initial_profile("Ajpar", Ajpar);
    if(ZeroElMass)
      dump.add(Ajpar, "Ajpar", 1); // output calculated Ajpar
  }

  if(log_density) {
    // Need to evolve the entire Ni, not perturbation
    Ni += Ni0; // Add background to perturbation
    Ni = log(Ni); // Take logarithm for starting point
  }

  // Set boundary conditions on jpar
  jpar.setBoundary("jpar");

  if(evolve_source) {
    bout_solve(Sn, "Sn");
  }
  if(input_source)
    mesh->get(Sn, "Sn");
  
  /************** SETUP COMMUNICATIONS **************/

  // add extra variables to communication
  comms.add(phi, Apar);

  // Add any other variables to be dumped to file
  dump.add(phi,  "phi",  1);
  dump.add(Apar, "Apar", 1);
  dump.add(jpar, "jpar", 1);

  SAVE_ONCE(Ni0);
  SAVE_ONCE(Te0);
  SAVE_ONCE(Ti0);

  SAVE_ONCE(Te_x);
  SAVE_ONCE(Ti_x);
  SAVE_ONCE(Ni_x);
  SAVE_ONCE(rho_s);
  SAVE_ONCE(wci);
  
  return(0);
}

// Routines for ExB terms (end of this file)
const Field2D vE_Grad(const Field2D &f, const Field2D &p);
const Field3D vE_Grad(const Field2D &f, const Field3D &p);
const Field3D vE_Grad(const Field3D &f, const Field2D &p);
const Field3D vE_Grad(const Field3D &f, const Field3D &p);

int physics_run(BoutReal t)
{
  if(log_density) {
    // Ni contains the logarithm of total density. 
    Ni = exp(Ni); // Convert to density
    Ni -= Ni0; // Turn into perturbation
  }

  // Invert vorticity to get phi
  
  // Solves \nabla^2_\perp x + (1./c)*\nabla_perp c\cdot\nabla_\perp x + a x = b
  // Arguments are:   (b,   bit-field, a,    c)
  // Passing NULL -> missing term
  if(nonlinear) {
    phi = invert_laplace(rho/(Ni0+Ni), phi_flags, NULL, &Ni0);
  }else
    phi = invert_laplace(rho/Ni0, phi_flags, NULL, &Ni0);

  if(estatic || ZeroElMass) {
    // Electrostatic operation
    Apar = 0.0;
  }else {
    // Invert Ajpar to get Apar
    static Field2D acoeff;
    static bool aset = false;
    
    if(!aset) // calculate Apar coefficient
      acoeff = (-0.5*beta_p/fmei)*Ni0;
    aset = true;
  
    Apar = invert_laplace(-acoeff*Ajpar, apar_flags, &acoeff);
  }

  // Communicate variables
  mesh->communicate(comms);

  // Update profiles
  if(nonlinear) {
    Nit = Ni0 + Ni;
    Tit = Ti0;
    Tet = Te0;
  }else {
    Nit = Ni0;
    Tit = Ti0;
    Tet = Te0;
  }

  BoutReal source_alpha;
  
  // Calculate source response
  if(source_converge > 0.) {
      source_alpha = source_response * exp(-1.*t/source_converge);
  }else
      source_alpha = source_response;

  // Update non-linear coefficients
  nu      = nu_hat * Nit / (Tet^1.5);
  mu_i    = mui_hat * Nit / (Tit^0.5);
  kapa_Te = 3.2*(1./fmei)*(wci/nueix)*(Tet^2.5);
  kapa_Ti = 3.9*(wci/nuiix)*(Tit^2.5);
  
  // Calculate pressures
  pei = (Tet+Tit)*Nit;
  pe  = Tet*Nit;
  
  if(ZeroElMass) {
    // Set jpar,Ve,Ajpar neglecting the electron inertia term
    //jpar = ((Te0*Grad_par(Ni, CELL_YLOW)) - (Ni0*Grad_par(phi, CELL_YLOW)))/(fmei*0.51*nu);
    jpar = ((Tet*Grad_par_LtoC(Ni)) - (Nit*Grad_par_LtoC(phi)))/(fmei*0.51*nu);
    
    // Set boundary condition on jpar
    jpar.applyBoundary();
    
    // Need to communicate jpar
    mesh->communicate(jpar);

    Ve = -jpar/Nit;
    Ajpar = Ve;
  }else {
    
    Ve = Ajpar + Apar;
    jpar = -Nit*Ve;
  }

  // DENSITY EQUATION

  ddt(Ni) = 0.0;
  if(evolve_ni) {
    ddt(Ni) -= vE_Grad(Ni0, phi);
    
    if(nonlinear)
      ddt(Ni) -= vE_Grad(Ni, phi);
    
    ddt(Ni) += Div_par_CtoL(jpar); // Left hand differencing

    if(evolve_source || input_source) {
      // Evolve source
      if(evolve_source)
        ddt(Sn) = mesh->averageY(-1. * source_alpha * Ni.DC() / Ni0);

      // Add density source/sink
      ddt(Ni) += Sn*where(Sn, Ni0, Nit); // Sn*Ni0 if Sn > 0, Sn*Nit if Sn < 0

    }else if(niprofile) {
      // Allowing Ni profile to change
      if(mesh->firstX()) {
	// Inner boundary
	for(int i=0;i<3;i++) {
	  // Relax upwards (only add density)
	  for(int j=0;j<mesh->ngy;j++)
	    for(int k=0;k<mesh->ngz;k++) {
	      if(Ni[i][j][k] < 0.0)
		ddt(Ni)[i][j][k] -= 0.1*Ni[i][j][k];
	      }
	    }
      }
      if(mesh->lastX()) {
	// Outer boundary
	for(int i=0;i<3;i++) {
	  // Relax downwards (only remove density)
	  for(int j=0;j<mesh->ngy;j++)
	    for(int k=0;k<mesh->ngz;k++) {
	      if(Ni[mesh->ngx-1-i][j][k] > 0.0)
		ddt(Ni)[mesh->ngx-1-i][j][k] -= 0.1*Ni[mesh->ngx-1-i][j][k];
	      }
	}
      }
    }else
      ddt(Ni) -= ddt(Ni).DC(); // REMOVE TOROIDAL AVERAGE DENSITY
    
    if(log_density) {
      // d/dt(ln Ni) = d/dt(Ni) / Ni
      ddt(Ni) /= Nit;
    }
  }

  // VORTICITY

  ddt(rho) = 0.0;
  if(evolve_rho) {
    
    if(nonlinear)
      ddt(rho) -= vE_Grad(rho, phi);

    //ddt(rho) += mesh->Bxy*mesh->Bxy*Div_par(jpar, CELL_CENTRE);
    ddt(rho) += mesh->Bxy*mesh->Bxy*Div_par_CtoL(jpar); // Left hand differencing

    if(nuIonNeutral > 0.0)
      ddt(rho) -= nuIonNeutral * rho;
    
    if(evolve_source || input_source) {
      // Sinks also remove vorticity
      ddt(rho) += Sn*where(Sn, 0., rho);
    }
  }
  
  // AJPAR

  ddt(Ajpar) = 0.0;
  if(evolve_ajpar) {

    //ddt(Ajpar) += (1./fmei)*Grad_par(phi, CELL_YLOW);
    ddt(Ajpar) += (1./fmei)*Grad_par_LtoC(phi); // Right-hand differencing

    //ddt(Ajpar) -= (1./fmei)*(Tet/Nit)*Grad_par(Ni, CELL_YLOW);
    ddt(Ajpar) -= (1./fmei)*(Tet/Nit)*Grad_par_LtoC(Ni);
    
    ddt(Ajpar) += 0.51*nu*jpar/Ni0;
  }

  // Z filtering
  if(filter_z) {
    // Filter out all except filter_z_mode
    
    ddt(rho) = filter(ddt(rho), filter_z_mode);
    ddt(Ni) = filter(ddt(Ni), filter_z_mode);
    ddt(Ajpar) = filter(ddt(Ajpar), filter_z_mode);
  }
  
  return 0;
}

/////////////////////////////////////////////////////////////////
// ExB terms. These routines allow comparisons with BOUT-06
// if bout_exb=true is set in BOUT.inp
/////////////////////////////////////////////////////////////////

const Field2D vE_Grad(const Field2D &f, const Field2D &p)
{
  Field2D result;
  if(bout_exb) {
    // Use a subset of terms for comparison to BOUT-06
    result = 0.0;
  }else {
    // Use full expression with all terms
    result = b0xGrad_dot_Grad(p, f) / mesh->Bxy;
  }
  return result;
}

const Field3D vE_Grad(const Field2D &f, const Field3D &p)
{
  Field3D result;
  if(arakawa) {
    // Arakawa scheme for perpendicular flow. Here as a test
    
    result.allocate();
    int ncz = mesh->ngz - 1;
    for(int jx=mesh->xstart;jx<=mesh->xend;jx++)
      for(int jy=mesh->ystart;jy<=mesh->yend;jy++)
        for(int jz=0;jz<ncz;jz++) {
          int jzp = (jz + 1) % ncz;
          int jzm = (jz - 1 + ncz) % ncz;
          
          // J++ = DDZ(p)*DDX(f) - DDX(p)*DDZ(f)
          BoutReal Jpp = 0.25*( (p[jx][jy][jzp] - p[jx][jy][jzm])*
                                (f[jx+1][jy] - f[jx-1][jy]) )
            / (mesh->dx[jx][jy] * mesh->dz);

          // J+x
          BoutReal Jpx = 0.25*( f[jx+1][jy]*(p[jx+1][jy][jzp]-p[jx+1][jy][jzm]) -
                                f[jx-1][jy]*(p[jx-1][jy][jzp]-p[jx-1][jy][jzm]) -
                                f[jx][jy]*(p[jx+1][jy][jzp]-p[jx-1][jy][jzp]) +
                                f[jx][jy]*(p[jx+1][jy][jzm]-p[jx-1][jy][jzm]))
            / (mesh->dx[jx][jy] * mesh->dz);
          // Jx+
          BoutReal Jxp = 0.25*( f[jx+1][jy]*(p[jx][jy][jzp]-p[jx+1][jy][jz]) -
                                f[jx-1][jy]*(p[jx-1][jy][jz]-p[jx][jy][jzm]) -
                                f[jx-1][jy]*(p[jx][jy][jzp]-p[jx-1][jy][jz]) +
                                f[jx+1][jy]*(p[jx+1][jy][jz]-p[jx][jy][jzm]))
            / (mesh->dx[jx][jy] * mesh->dz);
          
          result[jx][jy][jz] = (Jpp + Jpx + Jxp) / 3.;
        }
    
  }else if(bout_exb) {
    // Use a subset of terms for comparison to BOUT-06
    result = VDDX(DDZ(p), f);
  }else {
    // Use full expression with all terms
    result = b0xGrad_dot_Grad(p, f) / mesh->Bxy;
  }
  return result;
}

const Field3D vE_Grad(const Field3D &f, const Field2D &p)
{
  Field3D result;
  if(bout_exb) {
    // Use a subset of terms for comparison to BOUT-06
    result = VDDZ(-DDX(p), f);
  }else {
    // Use full expression with all terms
    result = b0xGrad_dot_Grad(p, f) / mesh->Bxy;
  }
  return result;
}

const Field3D vE_Grad(const Field3D &f, const Field3D &p)
{
  Field3D result;
  if(arakawa) {
    // Arakawa scheme for perpendicular flow. Here as a test
    
    result.allocate();
    
    int ncz = mesh->ngz - 1;
    for(int jx=mesh->xstart;jx<=mesh->xend;jx++)
      for(int jy=mesh->ystart;jy<=mesh->yend;jy++)
        for(int jz=0;jz<ncz;jz++) {
          int jzp = (jz + 1) % ncz;
          int jzm = (jz - 1 + ncz) % ncz;
          
          // J++ = DDZ(p)*DDX(f) - DDX(p)*DDZ(f)
          BoutReal Jpp = 0.25*( (p[jx][jy][jzp] - p[jx][jy][jzm])*
                                (f[jx+1][jy][jz] - f[jx-1][jy][jz]) -
                                (p[jx+1][jy][jz] - p[jx-1][jy][jz])*
                                (f[jx][jy][jzp] - f[jx][jy][jzm]) )
            / (mesh->dx[jx][jy] * mesh->dz);

          // J+x
          BoutReal Jpx = 0.25*( f[jx+1][jy][jz]*(p[jx+1][jy][jzp]-p[jx+1][jy][jzm]) -
                                f[jx-1][jy][jz]*(p[jx-1][jy][jzp]-p[jx-1][jy][jzm]) -
                                f[jx][jy][jzp]*(p[jx+1][jy][jzp]-p[jx-1][jy][jzp]) +
                                f[jx][jy][jzm]*(p[jx+1][jy][jzm]-p[jx-1][jy][jzm]))
            / (mesh->dx[jx][jy] * mesh->dz);
          // Jx+
          BoutReal Jxp = 0.25*( f[jx+1][jy][jzp]*(p[jx][jy][jzp]-p[jx+1][jy][jz]) -
                                f[jx-1][jy][jzm]*(p[jx-1][jy][jz]-p[jx][jy][jzm]) -
                                f[jx-1][jy][jzp]*(p[jx][jy][jzp]-p[jx-1][jy][jz]) +
                                f[jx+1][jy][jzm]*(p[jx+1][jy][jz]-p[jx][jy][jzm]))
            / (mesh->dx[jx][jy] * mesh->dz);
          
          result[jx][jy][jz] = (Jpp + Jpx + Jxp) / 3.;
        }
    
  }else if(bout_exb) {
    // Use a subset of terms for comparison to BOUT-06
    result = VDDX(DDZ(p), f) + VDDZ(-DDX(p), f);
  }else {
    // Use full expression with all terms
    result = b0xGrad_dot_Grad(p, f) / mesh->Bxy;
  }
  return result;
}
